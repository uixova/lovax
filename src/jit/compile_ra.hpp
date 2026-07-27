#ifndef LOVAX_JIT_COMPILE_RA_HPP
#define LOVAX_JIT_COMPILE_RA_HPP

// Register-allocating region compiler (RFC-027 Stage-4a/4b).
//
// Keeps values in registers across a whole loop region instead of the memory
// operand stack, and — the real win — keeps integers UNBOXED so chained ops pay
// no box/unbox. Stage-4a did pure-integer loops; Stage-4b adds list indexing so
// array kernels (qsort-style) run here too.
//
// Type state (static, per operand position and per pinned local):
//   VT_INT   — an unboxed int64 in a GP register.
//   VT_BOXED — a raw NaN-box word in a GP register (a list base, or a list
//              element whose element type is not known until it is used).
// A value is guarded+unboxed to VT_INT the moment it feeds an integer op
// (ensureInt), and boxed back only at the memory boundary (exit / bail). Because
// the region is int + list-of-anything, floats are Stage-4b-part-2.
//
// Locals are classified once by a pre-pass: a local used as the BASE of an
// index op is a list (VT_BOXED, loaded raw, never unboxed at entry); every other
// local is VT_INT (guarded + unboxed in the prologue). Operand positions are
// depth-indexed to a fixed register (position d -> pool[numLocals+d]), so every
// branch/merge is automatically consistent and internal jumps need no flush.
//
// Bail-to-interpreter is the template compiler's contract: box the live locals
// and operands back to memory (VT_INT -> box, VT_BOXED -> store the raw word),
// return the resume bytecode offset, the interpreter takes over. No deopt.
//
// GC safety for INDEX_SET: a region is CALL-free so it never allocates, but the
// incremental collector may be mid-MARK when the region is entered. Storing a
// heap POINTER into a list then needs a write barrier we cannot emit inline, so
// INDEX_SET of a value that carries a pointer BAILS to the interpreter (which
// stores it with the barrier). Storing an int/float/bool (no pointer) is safe
// and stays in machine code — which is exactly the array-of-numbers hot path.
//
// Scope: CALL-free, the opcode subset in raSupported(), int constants only, and
// it must fit the 7-register pool. Anything else falls back to the template
// compiler, which stays the differential oracle (RA == template == interpreter,
// bit-identical).
//
// Register file (CALL-free -> caller-saved regs are ours):
//   scratch : RAX, RCX, RDX
//   pool    : RSI, RDI, RBP, R8, R9, R10, R11   (pinned locals, then operands)
//   fixed   : RBX=slots R12=sp R13=globals R14=ctx R15=consts (RBP saved/restored)

#include <cstdint>
#include <cstddef>
#include <vector>
#include <unordered_map>
#include "compile.hpp"          // JitCtx, Region, JitFn, JIT_* constants, Asm

namespace Lovax {
namespace Jit {

inline bool jitRAEnabled = false;   // --jit-ra; default off -> zero regression risk

class RegionCompilerRA {
public:
    static constexpr Reg R_SLOTS   = RBX;
    static constexpr Reg R_SP      = R12;
    static constexpr Reg R_GLOBALS = R13;
    static constexpr Reg R_CTX     = R14;
    static constexpr Reg R_CONSTS  = R15;

    static constexpr int POOL_N = 7;
    static const Reg* pool() {
        static const Reg p[POOL_N] = { RSI, RDI, RBP, R8, R9, R10, R11 };
        return p;
    }
    static constexpr uint64_t TAG_INT     = JIT_TAGS | ((uint64_t)4 << JIT_TAG_SHIFT);
    static constexpr uint32_t TOP17_OBJ   = 0x1FFF5u;   // (TAGS | 5<<47) >> 47
    static constexpr uint32_t TOP17_BOXINT= 0x1FFF6u;   // (TAGS | 6<<47) >> 47
    // ListObject / std::vector<Value> layout (verified: object.hpp + libstdc++).
    static constexpr int32_t OBJ_TAG_OFF   = 8;    // Object::tag (low byte = enum)
    static constexpr int32_t ELEM_OFF      = 32;   // offsetof(ListObject, elements)
    static constexpr int32_t VEC_START_OFF = ELEM_OFF + 0;   // _M_start (Value*)
    static constexpr int32_t VEC_FINISH_OFF= ELEM_OFF + 8;   // _M_finish (Value*)
    static constexpr int     LIST_TAG      = 5;    // ObjectType::LIST

    enum VT { VT_INT = 0, VT_BOXED = 1 };

    const Chunk& chunk;
    size_t start, end;
    Asm a;
    std::unordered_map<size_t, Label> labels;
    Label prologueBail, bailTail;
    bool ok = true;

    std::vector<int> localSlots;
    std::unordered_map<int,int> slotToPool;
    std::vector<int> localVT;             // per pool index [0..numLocals): VT_INT / VT_BOXED
    int numLocals = 0;
    int maxDepth = 0;

    // emit-time operand type tracking, indexed by depth
    std::vector<int> otype;               // sized POOL_N; otype[d] valid for d < depth

    RegionCompilerRA(const Chunk& c, size_t s, size_t e)
        : chunk(c), start(s), end(e), otype(POOL_N, VT_INT) {}

    Label& labelAt(size_t off) { return labels[off]; }
    uint16_t rdU16(size_t off) const {
        return (uint16_t)((chunk.code[off] << 8) | chunk.code[off + 1]);
    }
    Reg localReg(int slot) { return pool()[slotToPool[slot]]; }
    Reg opReg(int depth)   { return pool()[numLocals + depth]; }

    // box a known-in-range unboxed int64 in `src` into a NaN-box word in `dst`
    // (RCX scratch only, RAX preserved).
    void boxTo(Reg dst, Reg src) {
        a.movRR(dst, src);
        a.movAbs(RCX, JIT_PAYLOAD); a.andRR(dst, RCX);
        a.movAbs(RCX, TAG_INT);     a.orRR(dst, RCX);
    }
    // bail: publish the live OPERANDS to the memory stack (VT_INT -> box,
    // VT_BOXED -> store the raw word), set RAX = resume offset, fall into the
    // tail (which writes the pinned locals). Every VT_INT value was range
    // checked, so boxing never overflows.
    void bailTo(size_t bytecodeOff, int depth) {
        for (int i = 0; i < depth; ++i) {
            if (otype[i] == VT_INT) boxTo(RDX, opReg(i));
            else                    a.movRR(RDX, opReg(i));
            a.movMR(R_SP, i * 8, RDX);
        }
        if (depth > 0) a.addRI(R_SP, depth * 8);
        a.movAbs(RAX, (uint64_t)bytecodeOff);
        a.jmp(bailTail);
    }
    void rangeCheck(size_t off, int depth) {         // RAX must still fit inline
        a.movRR(RCX, RAX); a.shlRI(RCX, 17); a.sarRI(RCX, 17);
        a.cmpRR(RCX, RAX);
        Label okL; a.jcc(E, okL);
        bailTo(off, depth);
        a.bind(okL);
    }
    // coerce operand at depth `d` to an unboxed int (guard + unbox), if boxed.
    void ensureInt(int d, size_t off, int depth) {
        if (otype[d] == VT_INT) return;
        Reg r = opReg(d);
        a.movRR(RCX, r); a.shrRI(RCX, JIT_TAG_SHIFT);
        a.cmpRI(RCX, (int32_t)JIT_TOP17_INT);
        Label okL; a.jcc(E, okL);
        bailTo(off, depth);
        a.bind(okL);
        a.shlRI(r, 17); a.sarRI(r, 17);
        otype[d] = VT_INT;
    }
    // compute &list[idx] into RAX from a boxed list word `objR` and unboxed int
    // `idxR`; bails on non-object, non-list, or out-of-range (incl. negative).
    void indexAddr(Reg objR, Reg idxR, size_t off, int depth) {
        a.movRR(RCX, objR); a.shrRI(RCX, JIT_TAG_SHIFT);
        a.cmpRI(RCX, (int32_t)TOP17_OBJ);              // is it a heap object?
        Label o1; a.jcc(E, o1); bailTo(off, depth); a.bind(o1);
        a.movAbs(RCX, JIT_PAYLOAD); a.movRR(RAX, objR); a.andRR(RAX, RCX);  // RAX = Object*
        a.movzxRM8(RCX, RAX, OBJ_TAG_OFF);             // low byte of tag
        a.cmpRI(RCX, LIST_TAG);
        Label o2; a.jcc(E, o2); bailTo(off, depth); a.bind(o2);
        a.movRM(RDX, RAX, VEC_START_OFF);              // RDX = data
        a.movRM(RCX, RAX, VEC_FINISH_OFF); a.subRR(RCX, RDX);  // RCX = size*8 bytes
        a.movRR(RAX, idxR); a.shlRI(RAX, 3);           // idx*8 (sign preserved)
        a.cmpRR(RAX, RCX);                             // unsigned: catches idx<0 too
        Label o3; a.jcc(B, o3); bailTo(off, depth); a.bind(o3);   // B = unsigned <
        a.addRR(RAX, RDX);                             // RAX = &list[idx]
    }

    bool compile();
};

inline bool raSupported(Op op) {
    switch (op) {
        case Op::GET_LOCAL: case Op::SET_LOCAL: case Op::CONST:
        case Op::POP: case Op::DUP:
        case Op::ADD: case Op::SUB: case Op::MUL:
        case Op::BIT_AND: case Op::BIT_OR: case Op::BIT_XOR:
        case Op::ADD_INPLACE:
        case Op::ADD_I: case Op::SUB_I: case Op::MUL_I: case Op::MOD_I:
        case Op::BAND_I: case Op::BOR_I: case Op::BXOR_I:
        case Op::LESS_JF: case Op::LESS_EQ_JF: case Op::GREATER_JF:
        case Op::GREATER_EQ_JF: case Op::EQUAL_JF: case Op::NOT_EQUAL_JF:
        case Op::INDEX_GET: case Op::INDEX_SET:
        case Op::JUMP: case Op::LOOP:
            return true;
        default:
            return false;
    }
}

inline int raStackDelta(Op op) {
    switch (op) {
        case Op::GET_LOCAL: case Op::CONST: case Op::DUP: return +1;
        case Op::SET_LOCAL: case Op::POP:                 return -1;
        case Op::ADD: case Op::SUB: case Op::MUL:
        case Op::BIT_AND: case Op::BIT_OR: case Op::BIT_XOR:
        case Op::ADD_INPLACE: case Op::INDEX_GET:         return -1;
        case Op::ADD_I: case Op::SUB_I: case Op::MUL_I: case Op::MOD_I:
        case Op::BAND_I: case Op::BOR_I: case Op::BXOR_I: return 0;
        case Op::LESS_JF: case Op::LESS_EQ_JF: case Op::GREATER_JF:
        case Op::GREATER_EQ_JF: case Op::EQUAL_JF: case Op::NOT_EQUAL_JF: return -2;
        case Op::INDEX_SET:                               return -3;
        default:                                          return 0;
    }
}

inline bool RegionCompilerRA::compile() {
    // ---- eligibility + local classification pre-pass ----
    // Track a small origin stack so we can tell which local is a list (used as
    // an index base). Origin -1 = "not a direct local".
    std::vector<int> orig;               // parallel to the operand stack
    auto opush = [&](int o){ orig.push_back(o); };
    auto opop  = [&](){ if (!orig.empty()) orig.pop_back(); };
    std::unordered_map<int,int> classify;  // slot -> VT (VT_BOXED if a list base)

    int depth = 0;
    for (size_t off = start; off < end; ) {
        Op op = (Op)chunk.code[off];
        int len = instrLength(chunk, off);
        if (len == 0 || !raSupported(op)) return false;
        if (off + len > end) return false;
        if (op == Op::CONST) {
            const Value& k = chunk.consts[rdU16(off + 1)];
            if (k.tag() != VKind::INT) return false;
            long long v = k.asInt();
            if (v < -(1ll << 46) || v >= (1ll << 46)) return false;
        }
        if (op == Op::GET_LOCAL || op == Op::SET_LOCAL) {
            int slot = (int)rdU16(off + 1);
            if (!slotToPool.count(slot)) { slotToPool[slot] = (int)localSlots.size(); localSlots.push_back(slot); }
        }
        // origin bookkeeping (explicit pops/pushes — net delta is NOT enough for
        // pop-2-push-1 ops). Read the index base BEFORE popping, then update.
        if (op == Op::INDEX_GET) {
            int b = (int)orig.size() - 2;                 // [.. base idx]
            if (b >= 0 && orig[b] >= 0) classify[orig[b]] = VT_BOXED;
        } else if (op == Op::INDEX_SET) {
            int b = (int)orig.size() - 3;                 // [.. base idx val]
            if (b >= 0 && orig[b] >= 0) classify[orig[b]] = VT_BOXED;
        }
        int d = raStackDelta(op);
        switch (op) {
            case Op::GET_LOCAL: opush((int)rdU16(off + 1)); break;
            case Op::CONST:     opush(-1); break;
            case Op::DUP:       opush(orig.empty() ? -1 : orig.back()); break;
            case Op::SET_LOCAL: case Op::POP: opop(); break;
            case Op::ADD: case Op::ADD_INPLACE: case Op::SUB: case Op::MUL:
            case Op::BIT_AND: case Op::BIT_OR: case Op::BIT_XOR:
            case Op::INDEX_GET: opop(); opop(); opush(-1); break;   // pop2 push1
            case Op::ADD_I: case Op::SUB_I: case Op::MUL_I: case Op::MOD_I:
            case Op::BAND_I: case Op::BOR_I: case Op::BXOR_I:
                opop(); opush(-1); break;                           // pop1 push1
            case Op::LESS_JF: case Op::LESS_EQ_JF: case Op::GREATER_JF:
            case Op::GREATER_EQ_JF: case Op::EQUAL_JF: case Op::NOT_EQUAL_JF:
                opop(); opop(); break;                              // pop2
            case Op::INDEX_SET: opop(); opop(); opop(); break;      // pop3
            case Op::JUMP: case Op::LOOP: break;
            default: break;
        }
        depth += d;
        if (depth < 0) return false;
        if (depth > maxDepth) maxDepth = depth;
        off += len;
    }
    numLocals = (int)localSlots.size();
    if (numLocals + maxDepth > POOL_N) return false;
    localVT.assign(numLocals, VT_INT);
    for (auto& kv : classify) localVT[slotToPool[kv.first]] = kv.second;

    // ---- prologue: save regs, load ctx, prepare pinned locals ----
    a.pushR(RBX); a.pushR(RBP); a.pushR(R12); a.pushR(R13); a.pushR(R14); a.pushR(R15);
    a.movRR(R_CTX, RDI);
    a.movRM(R_SLOTS,   R_CTX, offsetof(JitCtx, slots));
    a.movRM(R_SP,      R_CTX, offsetof(JitCtx, sp));
    a.movRM(R_GLOBALS, R_CTX, offsetof(JitCtx, globals));
    a.movRM(R_CONSTS,  R_CTX, offsetof(JitCtx, consts));
    for (int i = 0; i < numLocals; ++i) {
        Reg r = pool()[i];
        a.movRM(r, R_SLOTS, localSlots[i] * 8);
        if (localVT[i] == VT_INT) {
            a.movRR(RCX, r); a.shrRI(RCX, JIT_TAG_SHIFT);
            a.cmpRI(RCX, (int32_t)JIT_TOP17_INT);
            Label okL; a.jcc(E, okL); a.jmp(prologueBail); a.bind(okL);
            a.shlRI(r, 17); a.sarRI(r, 17);               // unbox
        }
        // VT_BOXED locals stay as the raw word (index ops guard at use)
    }

    // ---- emit pass ----
    depth = 0;
    for (size_t off = start; off < end; ) {
        Op op = (Op)chunk.code[off];
        int len = instrLength(chunk, off);
        a.bind(labelAt(off));
        size_t next = off + len;
        auto branchTo = [&](size_t target) {
            if (target >= start && target < end) a.jmp(labelAt(target));
            else bailTo(target, depth);
        };

        switch (op) {
            case Op::GET_LOCAL: {
                int slot = (int)rdU16(off + 1);
                a.movRR(opReg(depth), localReg(slot));
                otype[depth] = localVT[slotToPool[slot]];
                depth++; break;
            }
            case Op::SET_LOCAL: {
                int slot = (int)rdU16(off + 1);
                int lvt = localVT[slotToPool[slot]];
                if (lvt == VT_INT) ensureInt(depth - 1, off, depth);   // coerce value to int
                // VT_BOXED local <- whatever word is on top (raw copy)
                a.movRR(localReg(slot), opReg(depth - 1));
                depth--; break;
            }
            case Op::CONST: {
                long long v = chunk.consts[rdU16(off + 1)].asInt();
                a.movAbs(opReg(depth), (uint64_t)v);
                otype[depth] = VT_INT; depth++; break;
            }
            case Op::POP: depth--; break;
            case Op::DUP: a.movRR(opReg(depth), opReg(depth - 1)); otype[depth] = otype[depth - 1]; depth++; break;

            case Op::ADD: case Op::ADD_INPLACE: case Op::SUB: case Op::MUL:
            case Op::BIT_AND: case Op::BIT_OR: case Op::BIT_XOR: {
                ensureInt(depth - 2, off, depth);
                ensureInt(depth - 1, off, depth);
                Reg lhs = opReg(depth - 2), rhs = opReg(depth - 1);
                a.movRR(RAX, lhs);
                switch (op) {
                    case Op::ADD: case Op::ADD_INPLACE: a.addRR(RAX, rhs); break;
                    case Op::SUB: a.subRR(RAX, rhs); break;
                    case Op::MUL: a.imulRR(RAX, rhs); break;
                    case Op::BIT_AND: a.andRR(RAX, rhs); break;
                    case Op::BIT_OR:  a.orRR(RAX, rhs);  break;
                    case Op::BIT_XOR: a.xorRR(RAX, rhs); break;
                    default: break;
                }
                rangeCheck(off, depth);
                a.movRR(lhs, RAX);
                otype[depth - 2] = VT_INT;
                depth--; break;
            }

            case Op::ADD_I: case Op::SUB_I: case Op::MUL_I:
            case Op::BAND_I: case Op::BOR_I: case Op::BXOR_I: {
                ensureInt(depth - 1, off, depth);
                Reg r = opReg(depth - 1);
                int16_t k = (int16_t)rdU16(off + 1);
                a.movRR(RAX, r); a.movAbs(RDX, (uint64_t)(int64_t)k);
                switch (op) {
                    case Op::ADD_I:  a.addRR(RAX, RDX); break;
                    case Op::SUB_I:  a.subRR(RAX, RDX); break;
                    case Op::MUL_I:  a.imulRR(RAX, RDX); break;
                    case Op::BAND_I: a.andRR(RAX, RDX); break;
                    case Op::BOR_I:  a.orRR(RAX, RDX);  break;
                    case Op::BXOR_I: a.xorRR(RAX, RDX); break;
                    default: break;
                }
                rangeCheck(off, depth);
                a.movRR(r, RAX); break;
            }

            case Op::MOD_I: {
                ensureInt(depth - 1, off, depth);
                Reg r = opReg(depth - 1);
                int16_t k = (int16_t)rdU16(off + 1);
                if (k == 0) return false;
                a.movRR(RAX, r); a.movAbs(RCX, (uint64_t)(int64_t)k);
                a.cqo(); a.idivR(RCX); a.movRR(RAX, RDX);
                Label done;
                a.cmpRI(RAX, 0); a.jcc(E, done);
                a.movRR(RDX, RAX); a.xorRR(RDX, RCX);
                a.cmpRI(RDX, 0); a.jcc(GE, done);
                a.addRR(RAX, RCX);
                a.bind(done);
                a.movRR(r, RAX); break;
            }

            case Op::LESS_JF: case Op::LESS_EQ_JF:
            case Op::GREATER_JF: case Op::GREATER_EQ_JF:
            case Op::EQUAL_JF: case Op::NOT_EQUAL_JF: {
                ensureInt(depth - 2, off, depth);
                ensureInt(depth - 1, off, depth);
                uint16_t d = rdU16(off + 1);
                size_t target = next + d;
                Reg lhs = opReg(depth - 2), rhs = opReg(depth - 1);
                depth -= 2;
                a.cmpRR(lhs, rhs);
                Cond keep = op == Op::LESS_JF       ? L
                          : op == Op::LESS_EQ_JF    ? LE
                          : op == Op::GREATER_JF    ? G
                          : op == Op::GREATER_EQ_JF ? GE
                          : op == Op::EQUAL_JF      ? E : NE;
                Label fall;
                a.jcc(keep, fall);
                branchTo(target);
                a.bind(fall);
                break;
            }

            case Op::INDEX_GET: {
                ensureInt(depth - 1, off, depth);         // index must be int
                Reg objR = opReg(depth - 2), idxR = opReg(depth - 1);
                indexAddr(objR, idxR, off, depth);        // &elem -> RAX
                a.movRM(opReg(depth - 2), RAX, 0);        // load raw element word
                otype[depth - 2] = VT_BOXED;              // element type unknown
                depth--; break;
            }

            case Op::INDEX_SET: {
                ensureInt(depth - 2, off, depth);         // index must be int
                Reg objR = opReg(depth - 3), idxR = opReg(depth - 2), valR = opReg(depth - 1);
                if (otype[depth - 1] == VT_BOXED) {
                    // may carry a heap pointer -> would need a write barrier we
                    // cannot emit inline; bail so the interpreter stores it safely
                    a.movRR(RCX, valR); a.shrRI(RCX, JIT_TAG_SHIFT);
                    a.cmpRI(RCX, (int32_t)TOP17_OBJ);
                    Label s1; a.jcc(NE, s1); bailTo(off, depth); a.bind(s1);
                    a.movRR(RCX, valR); a.shrRI(RCX, JIT_TAG_SHIFT);
                    a.cmpRI(RCX, (int32_t)TOP17_BOXINT);
                    Label s2; a.jcc(NE, s2); bailTo(off, depth); a.bind(s2);
                }
                indexAddr(objR, idxR, off, depth);        // &elem -> RAX
                if (otype[depth - 1] == VT_INT) { boxTo(RDX, valR); a.movMR(RAX, 0, RDX); }
                else                            { a.movMR(RAX, 0, valR); }  // proven non-pointer word
                depth -= 3; break;
            }

            case Op::JUMP: branchTo(next + rdU16(off + 1)); break;
            case Op::LOOP: {
                size_t target = next - rdU16(off + 1);
                a.movAbs(RCX, (uint64_t)(uintptr_t)&Lovax::gcPending);
                a.movzxRM8(RAX, RCX, 0); a.cmpRI(RAX, 0);
                Label noGc; a.jcc(E, noGc);
                bailTo(target, depth);
                a.bind(noGc);
                branchTo(target);
                break;
            }
            default: return false;
        }
        if (!ok) return false;
        off = next;
    }

    // ---- normal exit: operand stack empty; resume at `end`; tail boxes locals
    a.movAbs(RAX, (uint64_t)end);
    a.jmp(bailTail);

    // ---- shared tail: RAX = resume offset; write pinned locals back ----
    a.bind(bailTail);
    for (int i = 0; i < numLocals; ++i) {
        if (localVT[i] == VT_INT) boxTo(RDX, pool()[i]);
        else                      a.movRR(RDX, pool()[i]);
        a.movMR(R_SLOTS, localSlots[i] * 8, RDX);
    }
    a.movMR(R_CTX, offsetof(JitCtx, sp), R_SP);
    a.popR(R15); a.popR(R14); a.popR(R13); a.popR(R12); a.popR(RBP); a.popR(RBX);
    a.ret();

    // ---- prologue bail: nothing modified in memory; resume at `start`
    a.bind(prologueBail);
    a.movMR(R_CTX, offsetof(JitCtx, sp), R_SP);
    a.movAbs(RAX, (uint64_t)start);
    a.popR(R15); a.popR(R14); a.popR(R13); a.popR(R12); a.popR(RBP); a.popR(RBX);
    a.ret();
    return true;
}

inline Region compileRegionRA(const Chunk& c, size_t start, size_t end) {
    Region r;
    RegionCompilerRA rc(c, start, end);
    if (!rc.compile()) return r;
    void* p = mcodeAlloc(rc.a.size());
    if (!p) return r;
    std::memcpy(p, rc.a.data(), rc.a.size());
    if (!mcodeFinalize(p, rc.a.size())) { mcodeRelease(p, rc.a.size()); return r; }
    r.fn = reinterpret_cast<JitFn>(p);
    r.code = p; r.codeSize = rc.a.size();
    r.startOff = start; r.endOff = end;
    return r;
}

} // namespace Jit
} // namespace Lovax

#endif // LOVAX_JIT_COMPILE_RA_HPP
