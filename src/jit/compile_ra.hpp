#ifndef LOVAX_JIT_COMPILE_RA_HPP
#define LOVAX_JIT_COMPILE_RA_HPP

// Register-allocating region compiler (RFC-027 Stage-4a).
//
// The template compiler (compile.hpp) keeps the operand stack in MEMORY and
// re-boxes every intermediate: each op does guardInt + unbox + unbox + compute
// + box (~25 instructions). That box/unbox churn — not the memory traffic — is
// what caps the JIT. This compiler removes it by keeping integers UNBOXED in
// registers across the whole region:
//
//   * A value is guarded to be an integer and unboxed ONCE, when it enters the
//     register domain: locals in the prologue, constants at compile time. From
//     then on the register holds a raw int64.
//   * Every op works on raw int64s directly (no per-op guard/unbox). After a
//     result that can grow (add/sub/mul/bitwise) we do a cheap RANGE CHECK —
//     does it still fit the 47-bit inline payload? — and bail if not. So a
//     per-op cost of ~25 instructions drops to ~6.
//   * Values are boxed back to the NaN-box word only at the MEMORY boundary:
//     normal region exit and any bail. Because every result was range-checked,
//     that boxing never overflows, so it is unconditional and cheap.
//
// Because the region is INT-ONLY, every register holds an unboxed int64 with no
// type ambiguity, and the depth-indexed operand mapping (operand position d is
// always pool[numLocals+d]) makes every branch/merge automatically consistent —
// internal jumps need no flush. Bail-to-interpreter is the same contract as the
// template compiler: box the live locals + operands to memory, return the
// bytecode offset, the interpreter takes over. No deopt machinery.
//
// Scope: CALL-free (a helper call would clobber the caller-saved value pool),
// the integer opcode subset in raSupported(), int constants only, and it must
// fit the pool (numLocals + maxDepth <= 7). Anything else falls back to the
// template compiler, which stays the correctness oracle (differential runs
// RA == template == interpreter, bit-identical).
//
// Register file (CALL-free -> caller-saved regs are ours):
//   scratch : RAX, RCX, RDX   (compute results, range check, idiv, guards, box)
//   pool    : RSI, RDI, RBP, R8, R9, R10, R11   (pinned locals, then operands)
//   fixed   : RBX=slots R12=sp R13=globals R14=ctx R15=consts (as template)
// RBP is callee-saved, so the prologue saves/restores it.

#include <cstdint>
#include <cstddef>
#include <vector>
#include <unordered_map>
#include "compile.hpp"          // JitCtx, Region, JitFn, JIT_* constants, Asm

namespace Lovax {
namespace Jit {

// Opt-in until proven: --jit-ra sets this. Default off -> zero regression risk.
inline bool jitRAEnabled = false;

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
    static constexpr uint64_t TAG_INT = JIT_TAGS | ((uint64_t)4 << JIT_TAG_SHIFT);

    const Chunk& chunk;
    size_t start, end;
    Asm a;
    std::unordered_map<size_t, Label> labels;
    Label prologueBail;                   // a local was not an int at entry
    Label bailTail;                       // shared bail/exit writeback
    bool ok = true;

    std::vector<int> localSlots;
    std::unordered_map<int,int> slotToPool;
    int numLocals = 0;
    int maxDepth = 0;

    RegionCompilerRA(const Chunk& c, size_t s, size_t e) : chunk(c), start(s), end(e) {}

    Label& labelAt(size_t off) { return labels[off]; }
    uint16_t rdU16(size_t off) const {
        return (uint16_t)((chunk.code[off] << 8) | chunk.code[off + 1]);
    }
    Reg localReg(int slot) { return pool()[slotToPool[slot]]; }
    Reg opReg(int depth)   { return pool()[numLocals + depth]; }

    // box a known-in-range unboxed int64 in `src` into the NaN-box word in `dst`.
    // Uses RCX as scratch, never RAX — so the resume offset in RAX is preserved.
    void boxTo(Reg dst, Reg src) {
        a.movRR(dst, src);
        a.movAbs(RCX, JIT_PAYLOAD); a.andRR(dst, RCX);
        a.movAbs(RCX, TAG_INT);     a.orRR(dst, RCX);
    }
    // bail: box the live OPERANDS to the memory stack, set RAX = resume offset,
    // fall into the tail (which boxes the pinned locals). Every live value is a
    // range-checked int64, so boxing never overflows. RAX (result of the failing
    // op) is discarded — we bail BEFORE committing it, operands still hold the
    // pre-op state the interpreter re-executes.
    void bailTo(size_t bytecodeOff, int depth) {
        for (int i = 0; i < depth; ++i) { boxTo(RDX, opReg(i)); a.movMR(R_SP, i * 8, RDX); }
        if (depth > 0) a.addRI(R_SP, depth * 8);
        a.movAbs(RAX, (uint64_t)bytecodeOff);
        a.jmp(bailTail);
    }
    // result (in RAX) must still fit the inline range, else bail to `off` with
    // the PRE-op operands intact (they are boxed at bail; interpreter re-does it)
    void rangeCheck(size_t off, int depth) {
        a.movRR(RCX, RAX); a.shlRI(RCX, 17); a.sarRI(RCX, 17);
        a.cmpRR(RCX, RAX);
        Label okL; a.jcc(E, okL);
        bailTo(off, depth);
        a.bind(okL);
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
        case Op::ADD_INPLACE:                             return -1;
        case Op::ADD_I: case Op::SUB_I: case Op::MUL_I: case Op::MOD_I:
        case Op::BAND_I: case Op::BOR_I: case Op::BXOR_I: return 0;
        case Op::LESS_JF: case Op::LESS_EQ_JF: case Op::GREATER_JF:
        case Op::GREATER_EQ_JF: case Op::EQUAL_JF: case Op::NOT_EQUAL_JF: return -2;
        default:                                          return 0;
    }
}

inline bool RegionCompilerRA::compile() {
    // ---- eligibility pre-pass ----
    int depth = 0;
    for (size_t off = start; off < end; ) {
        Op op = (Op)chunk.code[off];
        int len = instrLength(chunk, off);
        if (len == 0 || !raSupported(op)) return false;
        if (off + len > end) return false;
        if (op == Op::CONST) {
            // only inline-int constants (the value is baked unboxed at compile time)
            const Value& k = chunk.consts[rdU16(off + 1)];
            if (k.tag() != VKind::INT) return false;
            long long v = k.asInt();
            if (v < -(1ll << 46) || v >= (1ll << 46)) return false;   // boxed big int
        }
        if (op == Op::GET_LOCAL || op == Op::SET_LOCAL) {
            int slot = (int)rdU16(off + 1);
            if (!slotToPool.count(slot)) { slotToPool[slot] = (int)localSlots.size(); localSlots.push_back(slot); }
        }
        depth += raStackDelta(op);
        if (depth < 0) return false;
        if (depth > maxDepth) maxDepth = depth;
        off += len;
    }
    numLocals = (int)localSlots.size();
    if (numLocals + maxDepth > POOL_N) return false;

    // ---- prologue: save regs, load ctx, unbox pinned locals (guard once) ----
    a.pushR(RBX); a.pushR(RBP); a.pushR(R12); a.pushR(R13); a.pushR(R14); a.pushR(R15);
    a.movRR(R_CTX, RDI);
    a.movRM(R_SLOTS,   R_CTX, offsetof(JitCtx, slots));
    a.movRM(R_SP,      R_CTX, offsetof(JitCtx, sp));
    a.movRM(R_GLOBALS, R_CTX, offsetof(JitCtx, globals));
    a.movRM(R_CONSTS,  R_CTX, offsetof(JitCtx, consts));
    for (int i = 0; i < numLocals; ++i) {
        Reg r = pool()[i];
        a.movRM(r, R_SLOTS, localSlots[i] * 8);           // boxed local
        a.movRR(RCX, r); a.shrRI(RCX, JIT_TAG_SHIFT);
        a.cmpRI(RCX, (int32_t)JIT_TOP17_INT);
        Label okL; a.jcc(E, okL);
        a.jmp(prologueBail);                              // not an int -> give up cleanly
        a.bind(okL);
        a.shlRI(r, 17); a.sarRI(r, 17);                   // unbox in place -> raw int64
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
            case Op::GET_LOCAL: a.movRR(opReg(depth), localReg((int)rdU16(off + 1))); depth++; break;
            case Op::SET_LOCAL: a.movRR(localReg((int)rdU16(off + 1)), opReg(depth - 1)); depth--; break;
            case Op::CONST: {
                long long v = chunk.consts[rdU16(off + 1)].asInt();   // pre-pass proved inline int
                a.movAbs(opReg(depth), (uint64_t)v); depth++; break;
            }
            case Op::POP: depth--; break;
            case Op::DUP: a.movRR(opReg(depth), opReg(depth - 1)); depth++; break;

            case Op::ADD: case Op::ADD_INPLACE: case Op::SUB: case Op::MUL:
            case Op::BIT_AND: case Op::BIT_OR: case Op::BIT_XOR: {
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
                rangeCheck(off, depth);        // operands intact if it bails
                a.movRR(lhs, RAX);
                depth--;
                break;
            }

            case Op::ADD_I: case Op::SUB_I: case Op::MUL_I:
            case Op::BAND_I: case Op::BOR_I: case Op::BXOR_I: {
                Reg r = opReg(depth - 1);
                int16_t k = (int16_t)rdU16(off + 1);
                a.movRR(RAX, r);
                a.movAbs(RDX, (uint64_t)(int64_t)k);
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
                a.movRR(r, RAX);
                break;
            }

            case Op::MOD_I: {
                Reg r = opReg(depth - 1);
                int16_t k = (int16_t)rdU16(off + 1);
                if (k == 0) return false;
                a.movRR(RAX, r);
                a.movAbs(RCX, (uint64_t)(int64_t)k);
                a.cqo();
                a.idivR(RCX);                  // RDX = remainder
                a.movRR(RAX, RDX);
                Label done;                    // floor-mod adjust
                a.cmpRI(RAX, 0); a.jcc(E, done);
                a.movRR(RDX, RAX); a.xorRR(RDX, RCX);
                a.cmpRI(RDX, 0); a.jcc(GE, done);
                a.addRR(RAX, RCX);
                a.bind(done);
                a.movRR(r, RAX);               // |result| < |k| -> always fits, no range check
                break;
            }

            case Op::LESS_JF: case Op::LESS_EQ_JF:
            case Op::GREATER_JF: case Op::GREATER_EQ_JF:
            case Op::EQUAL_JF: case Op::NOT_EQUAL_JF: {
                uint16_t d = rdU16(off + 1);
                size_t target = next + d;
                Reg lhs = opReg(depth - 2), rhs = opReg(depth - 1);
                depth -= 2;
                a.cmpRR(lhs, rhs);             // both already unboxed int64
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

            case Op::JUMP: branchTo(next + rdU16(off + 1)); break;
            case Op::LOOP: {
                size_t target = next - rdU16(off + 1);
                a.movAbs(RCX, (uint64_t)(uintptr_t)&Lovax::gcPending);
                a.movzxRM8(RAX, RCX, 0);
                a.cmpRI(RAX, 0);
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

    // ---- normal exit: operand stack is empty here (depth 0); resume at `end`
    // and fall into the tail, which boxes the pinned locals back to memory ----
    a.movAbs(RAX, (uint64_t)end);
    a.jmp(bailTail);

    // ---- shared tail: RAX = resume offset. Box pinned locals to their memory
    // slots (RCX/RDX scratch keep RAX intact), publish sp, restore, return ----
    a.bind(bailTail);
    for (int i = 0; i < numLocals; ++i) { boxTo(RDX, pool()[i]); a.movMR(R_SLOTS, localSlots[i] * 8, RDX); }
    a.movMR(R_CTX, offsetof(JitCtx, sp), R_SP);
    a.popR(R15); a.popR(R14); a.popR(R13); a.popR(R12); a.popR(RBP); a.popR(RBX);
    a.ret();

    // ---- prologue bail: a local was not an int; nothing modified in memory
    // (we only unboxed into registers), so just restore and resume at `start` ----
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
