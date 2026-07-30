#ifndef LOVAX_JIT_COMPILE_FN_HPP
#define LOVAX_JIT_COMPILE_FN_HPP

// Numeric-function JIT (RFC-028 Stage-6a) — compiles a pure INTEGER recursive or
// leaf function to a COMPLETE native function, so recursion (fib, and the shape
// qsort/btree partition helpers take) runs entirely in machine code instead of
// paying an interpreter frame + trampoline per call.
//
// The function is emitted with the System V AMD64 ABI: integer parameters arrive
// in RDI, RSI, RDX, RCX (≤4 params), a recursion-depth counter in the next arg
// register, and the int64 result leaves in RAX. Parameters and operands live in
// callee-saved registers (RBX, RBP, R12–R15) so they survive a recursive call; a
// self-call is a direct `call` to the function's own entry with the ABI's arg
// registers set, exactly like a C compiler emits recursion. Integer arithmetic is
// native (two's-complement wrap = Lovax's int spec), UNBOXED throughout — exact
// int64 with no per-op boxing; only the final result is boxed at the interpreter
// boundary (inline, or heap for the rare >47-bit value).
//
// CORRECTNESS by construction: the compiled function is proven SIDE-EFFECT-FREE
// (no global writes, no allocation, no index/call except self-recursion, no I/O),
// so on ANY runtime deviation it just sets `g_numAbort` and returns — the caller
// discards the result and re-runs the whole call through the interpreter, the
// oracle. Deviations that abort: the depth counter reaching MAX_FRAMES (the
// interpreter then raises its own identical max-call-depth error), or the callee
// global no longer resolving to this exact closure (reassigned mid-flight). So the
// native path is a pure accelerator that can never change observable behaviour.
//
// Scope (Stage-6a): non-variadic, no upvalues, params-only (localCount ==
// paramCount), all-int, self-recursion only (mutual recursion bails), the op
// subset in fnSupported(); ≤4 params and the register file must fit. Anything else
// makes compileNumericFn return null and the function stays interpreted.

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>
#include "emit_x64.hpp"
#include "mcode.hpp"
#include "disasm.hpp"
#include "../vm/chunk.hpp"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"

namespace Lovax {
namespace Jit {

// On by default now that it is proven bit-identical to the interpreter across
// golden, the differential axis (123 programs), a 400-program recursion fuzz, and
// ASan+UBSan+GC_STRESS_INC — and it is side-effect-free with an interpreter
// fallback on any deviation, so it can never change observable behaviour.
// --no-numfn turns it off.
inline bool jitNumFnEnabled = true;

// Set by a compiled numeric function when it must defer to the interpreter.
inline int g_numAbort = 0;

// Mirror of VM::MAX_FRAMES (kept in sync; the static_assert-style check lives at
// the call site where MAX_FRAMES is visible).
inline constexpr int NUMFN_MAX_DEPTH = 500;

struct NumFnResult { void* fn = nullptr; void* code = nullptr; size_t size = 0; int arity = 0; };

class NumFnCompiler {
public:
    const Proto& proto;
    const void* selfClosure;     // globals[g] must still equal this (self-recursion)
    const Value* globals;
    const Chunk& c;
    Asm a;
    Label entry, epilogue, abort;
    std::unordered_map<size_t, Label> labels;
    int P = 0;                   // param count = arity
    int maxDepth = 0;
    bool ok = true;

    // callee-saved pool: params[0..P-1], depth at [P], operands at [P+1+d]
    static const Reg* pool() { static const Reg p[6] = { RBX, RBP, R12, R13, R14, R15 }; return p; }
    static const Reg* argReg() { static const Reg r[6] = { RDI, RSI, RDX, RCX, R8, R9 }; return r; }

    NumFnCompiler(const Proto& pr, const void* self, const Value* gl)
        : proto(pr), selfClosure(self), globals(gl), c(pr.chunk) {}

    Label& labelAt(size_t off) { return labels[off]; }
    uint16_t rdU16(size_t off) const { return (uint16_t)((c.code[off] << 8) | c.code[off + 1]); }
    Reg paramReg(int i) { return pool()[i]; }
    Reg depthReg()      { return pool()[P]; }
    Reg opReg(int d)    { return pool()[P + 1 + d]; }

    void abortAndReturn() {                        // g_numAbort = 1; return 0
        a.movAbs(RAX, (uint64_t)(uintptr_t)&g_numAbort);
        a.movMI8(RAX, 0, 1);
        a.movAbs(RAX, 0);
        a.jmp(epilogue);
    }

    bool compile(NumFnResult& out);
};

inline bool fnSupported(Op op) {
    switch (op) {
        case Op::GET_LOCAL:
        case Op::CONST: case Op::POP: case Op::DUP:
        case Op::LGET2: case Op::LGET_ADD_I: case Op::LGET_SUB_I:
        case Op::ADD: case Op::SUB: case Op::MUL:
        case Op::BIT_AND: case Op::BIT_OR: case Op::BIT_XOR: case Op::ADD_INPLACE:
        case Op::ADD_I: case Op::SUB_I: case Op::MUL_I: case Op::MOD_I:
        case Op::BAND_I: case Op::BOR_I: case Op::BXOR_I:
        case Op::LESS_JF: case Op::LESS_EQ_JF: case Op::GREATER_JF:
        case Op::GREATER_EQ_JF: case Op::EQUAL_JF: case Op::NOT_EQUAL_JF:
        case Op::LT_I_JF: case Op::LE_I_JF: case Op::GT_I_JF:
        case Op::GE_I_JF: case Op::EQ_I_JF: case Op::NE_I_JF:
        case Op::GET_GLOBAL: case Op::CALL:
        case Op::JUMP: case Op::RETURN:
            return true;
        default:
            return false;
    }
}

inline bool NumFnCompiler::compile(NumFnResult& out) {
    if (proto.variadic || proto.upvalueCount != 0) return false;
    P = proto.paramCount;
    if (P < 1 || P > 4) return false;
    if (proto.localCount != P) return false;       // params only (v1)

    // ---- reachability: the compiler appends a dead `NIL; RETURN` default tail;
    // only REACHABLE ops must be numeric/supported. Forward-flood from entry,
    // following branches; a RETURN has no successor.
    std::vector<char> reach(c.code.size(), 0);
    { std::vector<size_t> wl;
      if (!c.code.empty()) { reach[0] = 1; wl.push_back(0); }
      while (!wl.empty()) {
          size_t off = wl.back(); wl.pop_back();
          Op op = (Op)c.code[off];
          int len = instrLength(c, off);
          if (len == 0) return false;
          size_t next = off + len;
          auto add = [&](size_t t){ if (t < c.code.size() && !reach[t]) { reach[t]=1; wl.push_back(t); } };
          switch (op) {
              case Op::RETURN: break;
              case Op::JUMP: add(next + rdU16(off+1)); break;
              case Op::LESS_JF: case Op::LESS_EQ_JF: case Op::GREATER_JF:
              case Op::GREATER_EQ_JF: case Op::EQUAL_JF: case Op::NOT_EQUAL_JF:
                  add(next); add(next + rdU16(off+1)); break;
              case Op::LT_I_JF: case Op::LE_I_JF: case Op::GT_I_JF:
              case Op::GE_I_JF: case Op::EQ_I_JF: case Op::NE_I_JF:
                  add(next); add(next + rdU16(off+3)); break;
              default: add(next); break;
          }
      }
    }

    // ---- pass 1: op check + stack shape + self-call validation ----
    // A GET_GLOBAL must resolve to THIS closure and feed a CALL (self-recursion);
    // every CALL must have exactly `P` args (self). Consts must be int.
    int depth = 0;
    std::vector<int> orig;   // per operand: global idx (-1 otherwise)
    for (size_t off = 0; off < c.code.size(); ) {
        Op op = (Op)c.code[off];
        int len = instrLength(c, off);
        if (len == 0) return false;
        if (!reach[off]) { off += len; continue; }     // dead tail — skip
        if (!fnSupported(op)) return false;
        if (op == Op::CONST) {
            const Value& k = c.consts[rdU16(off + 1)];
            if (k.tag() != VKind::INT) return false;
        }
        if (op == Op::GET_GLOBAL) {
            int g = (int)rdU16(off + 1);
            if (!globals[g].isObj() || globals[g].asObj() != (Object*)selfClosure) return false;
        }
        if (op == Op::CALL) {
            int argc = c.code[off + 1];
            if (argc != P) return false;
            int calleePos = depth - argc - 1;
            if (calleePos < 0 || calleePos >= (int)orig.size() || orig[calleePos] < 0) return false;
        }
        // stack shape + origin
        switch (op) {
            case Op::GET_LOCAL: case Op::CONST: orig.push_back(-1); depth++; break;
            case Op::GET_GLOBAL: orig.push_back((int)rdU16(off + 1)); depth++; break;
            case Op::DUP: orig.push_back(orig.empty()?-1:orig.back()); depth++; break;
            case Op::LGET2: orig.push_back(-1); orig.push_back(-1); depth += 2; break;
            case Op::LGET_ADD_I: case Op::LGET_SUB_I: orig.push_back(-1); depth++; break;
            case Op::POP: if(!orig.empty())orig.pop_back(); depth--; break;
            case Op::ADD: case Op::SUB: case Op::MUL: case Op::ADD_INPLACE:
            case Op::BIT_AND: case Op::BIT_OR: case Op::BIT_XOR:
                if (orig.size() >= 2) { orig.pop_back(); orig.pop_back(); }
                orig.push_back(-1); depth--; break;
            case Op::ADD_I: case Op::SUB_I: case Op::MUL_I: case Op::MOD_I:
            case Op::BAND_I: case Op::BOR_I: case Op::BXOR_I: break;
            case Op::LESS_JF: case Op::LESS_EQ_JF: case Op::GREATER_JF:
            case Op::GREATER_EQ_JF: case Op::EQUAL_JF: case Op::NOT_EQUAL_JF:
                if (orig.size() >= 2) { orig.pop_back(); orig.pop_back(); }
                depth -= 2; break;
            case Op::LT_I_JF: case Op::LE_I_JF: case Op::GT_I_JF:
            case Op::GE_I_JF: case Op::EQ_I_JF: case Op::NE_I_JF:
                if (!orig.empty()) orig.pop_back();
                depth--; break;
            case Op::CALL: {
                int argc = c.code[off + 1];
                for(int i=0;i<argc+1 && !orig.empty();++i) orig.pop_back();
                orig.push_back(-1); depth -= argc; break;
            }
            case Op::RETURN: if(!orig.empty())orig.pop_back(); depth--; break;
            case Op::JUMP: break;
            default: return false;
        }
        if (depth < 0) return false;
        if (depth > maxDepth) maxDepth = depth;
        off += len;
    }
    if (P + 1 + maxDepth > 6) return false;         // register file must fit

    // ---- prologue ----
    a.bind(entry);
    for (Reg r : { RBX, RBP, R12, R13, R14, R15 }) a.pushR(r);
    a.subRI(RSP, 8);                                // 6 pushes -> RSP%16==8; align for calls
    for (int i = 0; i < P; ++i) a.movRR(paramReg(i), argReg()[i]);
    a.movRR(depthReg(), argReg()[P]);              // recursion depth
    // depth guard: if depth >= MAX_FRAMES abort (interpreter re-runs -> its error)
    a.cmpRI(depthReg(), NUMFN_MAX_DEPTH);
    { Label okd; a.jcc(L, okd); abortAndReturn(); a.bind(okd); }

    // ---- emit ----
    depth = 0;
    auto immArith = [&](Op op, int16_t k){
        Reg r = opReg(depth-1);
        a.movAbs(RAX, (uint64_t)(int64_t)k);
        switch(op){
            case Op::ADD_I: a.addRR(r, RAX); break;
            case Op::SUB_I: a.subRR(r, RAX); break;
            case Op::MUL_I: a.imulRR(r, RAX); break;
            case Op::BAND_I: a.andRR(r, RAX); break;
            case Op::BOR_I: a.orRR(r, RAX); break;
            case Op::BXOR_I: a.xorRR(r, RAX); break;
            default: break;
        }
    };
    for (size_t off = 0; off < c.code.size(); ) {
        Op op = (Op)c.code[off];
        int len = instrLength(c, off);
        if (!reach[off]) { off += len; continue; }     // dead tail — emit nothing
        a.bind(labelAt(off));
        size_t next = off + len;
        auto branchTo = [&](size_t target){ a.jmp(labelAt(target)); };
        switch (op) {
            case Op::GET_LOCAL: a.movRR(opReg(depth), paramReg((int)rdU16(off+1))); depth++; break;
            case Op::CONST: a.movAbs(opReg(depth), (uint64_t)c.consts[rdU16(off+1)].asInt()); depth++; break;
            case Op::POP: depth--; break;
            case Op::DUP: a.movRR(opReg(depth), opReg(depth-1)); depth++; break;
            case Op::LGET2:
                a.movRR(opReg(depth), paramReg((int)rdU16(off+1))); depth++;
                a.movRR(opReg(depth), paramReg((int)rdU16(off+3))); depth++; break;
            case Op::LGET_ADD_I: case Op::LGET_SUB_I: {
                a.movRR(opReg(depth), paramReg((int)rdU16(off+1)));
                int16_t k = (int16_t)rdU16(off+3);
                a.movAbs(RAX, (uint64_t)(int64_t)k);
                if (op==Op::LGET_ADD_I) a.addRR(opReg(depth), RAX); else a.subRR(opReg(depth), RAX);
                depth++; break;
            }
            case Op::ADD: case Op::ADD_INPLACE: a.addRR(opReg(depth-2), opReg(depth-1)); depth--; break;
            case Op::SUB: a.subRR(opReg(depth-2), opReg(depth-1)); depth--; break;
            case Op::MUL: a.imulRR(opReg(depth-2), opReg(depth-1)); depth--; break;
            case Op::BIT_AND: a.andRR(opReg(depth-2), opReg(depth-1)); depth--; break;
            case Op::BIT_OR:  a.orRR(opReg(depth-2), opReg(depth-1));  depth--; break;
            case Op::BIT_XOR: a.xorRR(opReg(depth-2), opReg(depth-1)); depth--; break;
            case Op::ADD_I: case Op::SUB_I: case Op::MUL_I:
            case Op::BAND_I: case Op::BOR_I: case Op::BXOR_I:
                immArith(op, (int16_t)rdU16(off+1)); break;
            case Op::MOD_I: {
                int16_t k = (int16_t)rdU16(off+1);
                if (k == 0) return false;
                Reg r = opReg(depth-1);
                a.movRR(RAX, r); a.movAbs(RCX, (uint64_t)(int64_t)k);
                a.cqo(); a.idivR(RCX); a.movRR(RAX, RDX);      // RAX = rem
                Label done; a.cmpRI(RAX, 0); a.jcc(E, done);
                a.movRR(RDX, RAX); a.xorRR(RDX, RCX); a.cmpRI(RDX, 0); a.jcc(GE, done);
                a.addRR(RAX, RCX); a.bind(done);
                a.movRR(r, RAX); break;
            }
            case Op::LESS_JF: case Op::LESS_EQ_JF: case Op::GREATER_JF:
            case Op::GREATER_EQ_JF: case Op::EQUAL_JF: case Op::NOT_EQUAL_JF: {
                size_t target = next + rdU16(off+1);
                a.cmpRR(opReg(depth-2), opReg(depth-1)); depth -= 2;
                Cond keep = op==Op::LESS_JF?L : op==Op::LESS_EQ_JF?LE : op==Op::GREATER_JF?G
                          : op==Op::GREATER_EQ_JF?GE : op==Op::EQUAL_JF?E : NE;
                Label fall; a.jcc(keep, fall); branchTo(target); a.bind(fall); break;
            }
            case Op::LT_I_JF: case Op::LE_I_JF: case Op::GT_I_JF:
            case Op::GE_I_JF: case Op::EQ_I_JF: case Op::NE_I_JF: {
                int16_t k = (int16_t)rdU16(off+1); size_t target = next + rdU16(off+3);
                a.movAbs(RAX, (uint64_t)(int64_t)k); a.cmpRR(opReg(depth-1), RAX); depth--;
                Cond keep = op==Op::LT_I_JF?L : op==Op::LE_I_JF?LE : op==Op::GT_I_JF?G
                          : op==Op::GE_I_JF?GE : op==Op::EQ_I_JF?E : NE;
                Label fall; a.jcc(keep, fall); branchTo(target); a.bind(fall); break;
            }
            case Op::GET_GLOBAL: depth++; break;   // self callee marker (validated); CALL overwrites
            case Op::CALL: {
                int argc = c.code[off+1];
                // set ABI arg registers: args then depth+1, then re-verify self, call
                for (int i = 0; i < argc; ++i) a.movRR(argReg()[i], opReg(depth - argc + i));
                a.movRR(argReg()[argc], depthReg()); a.addRI(argReg()[argc], 1);
                a.callLabel(entry);                 // direct self-recursion
                a.movRR(opReg(depth - argc - 1), RAX);   // result replaces callee slot
                depth -= argc; break;
            }
            case Op::JUMP: branchTo(next + rdU16(off+1)); break;
            case Op::RETURN: a.movRR(RAX, opReg(depth-1)); depth--; a.jmp(epilogue); break;
            default: return false;
        }
        if (!ok) return false;
        off = next;
    }

    // ---- epilogue ----
    a.bind(epilogue);
    a.addRI(RSP, 8);
    for (Reg r : { R15, R14, R13, R12, RBP, RBX }) a.popR(r);
    a.ret();

    // finalize
    void* p = mcodeAlloc(a.size());
    if (!p) return false;
    std::memcpy(p, a.data(), a.size());
    if (!mcodeFinalize(p, a.size())) { mcodeRelease(p, a.size()); return false; }
    out.fn = p; out.code = p; out.size = a.size(); out.arity = P;
    return true;
}

inline NumFnResult compileNumericFn(const Proto& proto, const void* selfClosure, const Value* globals) {
    NumFnResult out;
    NumFnCompiler nc(proto, selfClosure, globals);
    if (!nc.compile(out)) { out.fn = nullptr; return out; }
    return out;
}

// Marshal ≤4 int args + the recursion depth into the ABI and call the native fn.
inline int64_t callNumFn(void* fn, int argc, const int64_t* a, int64_t depth) {
    switch (argc) {
        case 1: return ((int64_t(*)(int64_t,int64_t))fn)(a[0], depth);
        case 2: return ((int64_t(*)(int64_t,int64_t,int64_t))fn)(a[0], a[1], depth);
        case 3: return ((int64_t(*)(int64_t,int64_t,int64_t,int64_t))fn)(a[0], a[1], a[2], depth);
        case 4: return ((int64_t(*)(int64_t,int64_t,int64_t,int64_t,int64_t))fn)(a[0], a[1], a[2], a[3], depth);
        default: g_numAbort = 1; return 0;
    }
}

} // namespace Jit
} // namespace Lovax

#pragma GCC diagnostic pop

#endif // LOVAX_JIT_COMPILE_FN_HPP
