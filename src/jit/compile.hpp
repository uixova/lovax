#ifndef LOVAX_JIT_COMPILE_HPP
#define LOVAX_JIT_COMPILE_HPP

// Baseline region compiler (RFC-026 Stage 2).
//
// Compiles a bytecode RANGE (a hot loop) to machine code. The design rests on
// one property: compiled code keeps the value stack in memory in EXACTLY the
// interpreter's layout. That makes entry and exit free — no register state to
// reconstruct — so on any unexpected condition (a non-int operand, an int that
// no longer fits inline, a pending GC, an opcode we don't handle at run time)
// the compiled code simply writes `sp` back and RETURNS THE BYTECODE OFFSET of
// the instruction that gave up. The interpreter resumes there and does the work
// itself. Consequences:
//   * no slow-path helpers and no deoptimisation machinery in this stage;
//   * correctness is inherited from the interpreter — compiled code only ever
//     handles the cases it can prove, and defers everything else;
//   * a guard must therefore fire BEFORE the stack is mutated, so every opcode
//     below reads its operands without moving `sp` and commits at the end.
//
// Value layout (RFC-024, 8-byte NaN-box): the top 17 bits are the tag.
//   value >> 47 == 0x1FFF4  <=>  inline integer (TAGS | T_INT<<47)
//   unbox: shl 17, sar 17   (sign-extend from bit 46)
//   box:   and PAYLOAD, or TAG_INT   (only valid if it round-trips — checked)

#include <cstdint>
#include <cstddef>
#include <vector>
#include <unordered_map>
#include "emit_x64.hpp"
#include "mcode.hpp"
#include "../vm/chunk.hpp"

namespace Lovax {
namespace Jit {

// Everything compiled code needs. The VM fills this in before entering.
struct JitCtx {
    Value* slots;          // frame locals base
    Value* sp;             // operand stack top (read in, written back out)
    Value* globals;        // effective global table base
    const Value* consts;   // chunk constant pool
    const unsigned char* globalDefined;
};

// Returns the bytecode offset to resume at.
using JitFn = int64_t (*)(JitCtx*);

struct Region {
    JitFn fn = nullptr;
    void* code = nullptr;
    size_t codeSize = 0;
};

// ---- NaN-box constants mirrored for codegen (must match src/vm/value.hpp) ----
static constexpr uint64_t JIT_TAGS      = 0xFFF8000000000000ull;
static constexpr uint64_t JIT_PAYLOAD   = 0x00007FFFFFFFFFFFull;
static constexpr int      JIT_TAG_SHIFT = 47;
static constexpr uint32_t JIT_TOP17_INT = 0x1FFF4u;   // (TAGS | 4<<47) >> 47

class RegionCompiler {
public:
    // Registers held across the whole region (all callee-saved, so helper-free
    // compiled code never has to spill them).
    static constexpr Reg R_SLOTS   = RBX;
    static constexpr Reg R_SP      = R12;
    static constexpr Reg R_GLOBALS = R13;
    static constexpr Reg R_CTX     = R14;
    static constexpr Reg R_CONSTS  = R15;

    const Chunk& chunk;
    size_t start, end;
    Asm a;
    std::unordered_map<size_t, Label> labels;   // bytecode offset -> machine label
    Label epilogue;
    bool ok = true;

    RegionCompiler(const Chunk& c, size_t s, size_t e) : chunk(c), start(s), end(e) {}

    Label& labelAt(size_t off) { return labels[off]; }

    // ---- helpers ----
    uint16_t rdU16(size_t off) const {
        return (uint16_t)((chunk.code[off] << 8) | chunk.code[off + 1]);
    }
    // Leave the region: RAX = bytecode offset the interpreter must resume at.
    void bailTo(size_t bytecodeOff) {
        a.movAbs(RAX, (uint64_t)bytecodeOff);
        a.jmp(epilogue);
    }
    // Guard: value in `r` must be an inline integer, else bail to `off`.
    void guardInt(Reg r, size_t off, Reg scratch = RCX) {
        a.movRR(scratch, r);
        a.shrRI(scratch, JIT_TAG_SHIFT);
        a.cmpRI(scratch, (int32_t)JIT_TOP17_INT);
        Label okL;
        a.jcc(E, okL);
        bailTo(off);
        a.bind(okL);
    }
    void unboxInt(Reg r) { a.shlRI(r, 17); a.sarRI(r, 17); }
    // Box the int in `r`; bails to `off` if it does not fit the inline range
    // (the interpreter will heap-box it — that path allocates, so it must not
    // happen inside compiled code).
    void boxInt(Reg r, size_t off, Reg scratch = RCX) {
        a.movRR(scratch, r);
        a.shlRI(scratch, 17);
        a.sarRI(scratch, 17);
        a.cmpRR(scratch, r);
        Label fits;
        a.jcc(E, fits);
        bailTo(off);
        a.bind(fits);
        a.movAbs(RDX, JIT_PAYLOAD);
        a.andRR(r, RDX);
        a.movAbs(RDX, JIT_TAGS | ((uint64_t)4 << JIT_TAG_SHIFT));
        a.orRR(r, RDX);
    }
    // stack helpers (sp points one past the top)
    void pushReg(Reg r)  { a.movMR(R_SP, 0, r); a.addRI(R_SP, 8); }
    void popReg(Reg r)   { a.subRI(R_SP, 8); a.movRM(r, R_SP, 0); }
    void peekReg(Reg r, int depth) { a.movRM(r, R_SP, -8 * (depth + 1)); }
    void pokeReg(int depth, Reg r) { a.movMR(R_SP, -8 * (depth + 1), r); }

    // ---- prologue / epilogue ----
    void prologue() {
        a.pushR(RBX); a.pushR(R12); a.pushR(R13); a.pushR(R14); a.pushR(R15);
        // 5 pushes: entry RSP%16==8 -> 8+40=48 -> 16-byte aligned for any call.
        a.movRR(R_CTX, RDI);
        a.movRM(R_SLOTS,   R_CTX, offsetof(JitCtx, slots));
        a.movRM(R_SP,      R_CTX, offsetof(JitCtx, sp));
        a.movRM(R_GLOBALS, R_CTX, offsetof(JitCtx, globals));
        a.movRM(R_CONSTS,  R_CTX, offsetof(JitCtx, consts));
    }
    void emitEpilogue() {
        a.bind(epilogue);
        a.movMR(R_CTX, offsetof(JitCtx, sp), R_SP);   // publish sp back
        a.popR(R15); a.popR(R14); a.popR(R13); a.popR(R12); a.popR(RBX);
        a.ret();
    }

    // ---- integer binary op on the two stack top values, in place ----
    // op: 0=add 1=sub 2=mul 3=and 4=or 5=xor
    void intBinary(int op, size_t off) {
        peekReg(RAX, 1);              // lhs
        guardInt(RAX, off);
        peekReg(RSI, 0);              // rhs
        guardInt(RSI, off, RDX);
        unboxInt(RAX); unboxInt(RSI);
        switch (op) {
            case 0: a.addRR(RAX, RSI); break;
            case 1: a.subRR(RAX, RSI); break;
            case 2: a.imulRR(RAX, RSI); break;
            case 3: a.andRR(RAX, RSI); break;
            case 4: a.orRR(RAX, RSI);  break;
            case 5: a.xorRR(RAX, RSI); break;
        }
        boxInt(RAX, off);
        a.subRI(R_SP, 8);             // consume rhs
        pokeReg(0, RAX);              // overwrite lhs with the result
    }

    // ---- immediate arithmetic on the stack top, in place (ADD_I/SUB_I/...) ----
    void immArith(int op, int16_t k, size_t off) {
        peekReg(RAX, 0);
        guardInt(RAX, off);
        unboxInt(RAX);
        a.movAbs(RSI, (uint64_t)(int64_t)k);
        switch (op) {
            case 0: a.addRR(RAX, RSI); break;
            case 1: a.subRR(RAX, RSI); break;
            case 2: a.imulRR(RAX, RSI); break;
            case 3: a.andRR(RAX, RSI); break;
            case 4: a.orRR(RAX, RSI);  break;
            case 5: a.xorRR(RAX, RSI); break;
        }
        boxInt(RAX, off);
        pokeReg(0, RAX);
    }

    // MOD_I: floor-mod with the divisor's sign (Python/Lua rule), k != 0.
    void immMod(int16_t k, size_t off) {
        if (k == 0) { ok = false; return; }
        peekReg(RAX, 0);
        guardInt(RAX, off);
        unboxInt(RAX);
        a.movAbs(RSI, (uint64_t)(int64_t)k);
        a.cqo();
        a.idivR(RSI);                 // RDX = remainder
        a.movRR(RAX, RDX);
        // if (m != 0 && (m<0) != (k<0)) m += k;
        Label done;
        a.cmpRI(RAX, 0);
        a.jcc(E, done);
        a.movRR(RCX, RAX);
        a.xorRR(RCX, RSI);            // sign differs iff the xor is negative
        a.cmpRI(RCX, 0);
        a.jcc(GE, done);
        a.addRR(RAX, RSI);
        a.bind(done);
        boxInt(RAX, off);
        pokeReg(0, RAX);
    }

    bool compile();
};

// Opcodes the Stage-2 code generator can emit. Anything else in the range makes
// the whole region fall back to the interpreter (no partial compilation).
inline bool jitSupported(Op op) {
    switch (op) {
        case Op::GET_LOCAL: case Op::SET_LOCAL: case Op::CONST:
        case Op::POP: case Op::DUP: case Op::CLOSE_UPVALUE:
        case Op::GET_GLOBAL: case Op::SET_GLOBAL:
        case Op::ADD: case Op::SUB: case Op::MUL:
        case Op::BIT_AND: case Op::BIT_OR: case Op::BIT_XOR:
        case Op::ADD_INPLACE:
        case Op::ADD_I: case Op::SUB_I: case Op::MUL_I: case Op::MOD_I:
        case Op::BAND_I: case Op::BOR_I: case Op::BXOR_I:
        case Op::LGET2: case Op::LGET_ADD_I: case Op::LGET_SUB_I:
        case Op::LESS_JF: case Op::LESS_EQ_JF: case Op::GREATER_JF:
        case Op::GREATER_EQ_JF:
        case Op::JUMP: case Op::LOOP:
            return true;
        default:
            return false;
    }
}

inline bool RegionCompiler::compile() {
    // ---- pass 1: every opcode supported, and instruction lengths known ----
    for (size_t off = start; off < end; ) {
        Op op = (Op)chunk.code[off];
        int len = instrLength(chunk, off);
        if (len == 0 || !jitSupported(op)) return false;
        if (off + len > end) return false;          // instruction straddles the end
        off += len;
    }

    prologue();

    // ---- pass 2: emit ----
    for (size_t off = start; off < end; ) {
        Op op = (Op)chunk.code[off];
        int len = instrLength(chunk, off);
        a.bind(labelAt(off));
        size_t next = off + len;

        // Jump to a bytecode offset: internal -> machine label, external -> leave.
        auto branchTo = [&](size_t target) {
            if (target >= start && target < end) a.jmp(labelAt(target));
            else bailTo(target);
        };

        switch (op) {
            case Op::GET_LOCAL: {
                a.movRM(RAX, R_SLOTS, (int32_t)rdU16(off + 1) * 8);
                pushReg(RAX);
                break;
            }
            case Op::SET_LOCAL: {
                popReg(RAX);
                a.movMR(R_SLOTS, (int32_t)rdU16(off + 1) * 8, RAX);
                break;
            }
            case Op::CONST: {
                a.movRM(RAX, R_CONSTS, (int32_t)rdU16(off + 1) * 8);
                pushReg(RAX);
                break;
            }
            case Op::POP: a.subRI(R_SP, 8); break;
            case Op::DUP: peekReg(RAX, 0); pushReg(RAX); break;
            // Safe as a no-op: the VM refuses to enter a region while any
            // upvalue is open, and no opcode here can open one (CLOSURE is
            // unsupported), so there is never anything to close.
            case Op::CLOSE_UPVALUE: break;

            case Op::GET_GLOBAL: case Op::SET_GLOBAL: {
                uint16_t s = rdU16(off + 1);
                a.movRM(RCX, R_CTX, offsetof(JitCtx, globalDefined));
                a.movzxRM8(RAX, RCX, (int32_t)s);   // globalDefined[s], one byte
                a.cmpRI(RAX, 0);
                Label defined;
                a.jcc(NE, defined);
                bailTo(off);                        // undefined -> interpreter errors
                a.bind(defined);
                if (op == Op::GET_GLOBAL) {
                    a.movRM(RAX, R_GLOBALS, (int32_t)s * 8);
                    pushReg(RAX);
                } else {
                    popReg(RAX);
                    a.movMR(R_GLOBALS, (int32_t)s * 8, RAX);
                }
                break;
            }

            case Op::ADD: case Op::ADD_INPLACE: intBinary(0, off); break;
            case Op::SUB:      intBinary(1, off); break;
            case Op::MUL:      intBinary(2, off); break;
            case Op::BIT_AND:  intBinary(3, off); break;
            case Op::BIT_OR:   intBinary(4, off); break;
            case Op::BIT_XOR:  intBinary(5, off); break;

            case Op::ADD_I:  immArith(0, (int16_t)rdU16(off + 1), off); break;
            case Op::SUB_I:  immArith(1, (int16_t)rdU16(off + 1), off); break;
            case Op::MUL_I:  immArith(2, (int16_t)rdU16(off + 1), off); break;
            case Op::BAND_I: immArith(3, (int16_t)rdU16(off + 1), off); break;
            case Op::BOR_I:  immArith(4, (int16_t)rdU16(off + 1), off); break;
            case Op::BXOR_I: immArith(5, (int16_t)rdU16(off + 1), off); break;
            case Op::MOD_I:  immMod((int16_t)rdU16(off + 1), off); break;

            case Op::LGET2: {
                a.movRM(RAX, R_SLOTS, (int32_t)rdU16(off + 1) * 8); pushReg(RAX);
                a.movRM(RAX, R_SLOTS, (int32_t)rdU16(off + 3) * 8); pushReg(RAX);
                break;
            }
            case Op::LGET_ADD_I: case Op::LGET_SUB_I: {
                a.movRM(RAX, R_SLOTS, (int32_t)rdU16(off + 1) * 8);
                pushReg(RAX);
                immArith(op == Op::LGET_ADD_I ? 0 : 1, (int16_t)rdU16(off + 3), off);
                break;
            }

            case Op::LESS_JF: case Op::LESS_EQ_JF:
            case Op::GREATER_JF: case Op::GREATER_EQ_JF: {
                uint16_t d = rdU16(off + 1);
                size_t target = next + d;             // taken when the test is FALSE
                peekReg(RAX, 1); guardInt(RAX, off);
                peekReg(RSI, 0); guardInt(RSI, off, RDX);
                unboxInt(RAX); unboxInt(RSI);
                a.subRI(R_SP, 16);                    // both operands consumed
                a.cmpRR(RAX, RSI);
                Cond keep = op == Op::LESS_JF       ? L
                          : op == Op::LESS_EQ_JF    ? LE
                          : op == Op::GREATER_JF    ? G : GE;
                Label fall;
                a.jcc(keep, fall);                    // condition true -> fall through
                branchTo(target);
                a.bind(fall);
                break;
            }

            case Op::JUMP: branchTo(next + rdU16(off + 1)); break;
            case Op::LOOP: {
                size_t target = next - rdU16(off + 1);
                // Back-edge safepoint: if the collector is waiting, hand control
                // back so the interpreter runs it (compiled code never collects).
                a.movAbs(RCX, (uint64_t)(uintptr_t)&Lovax::gcPending);
                a.movzxRM8(RAX, RCX, 0);            // bool is one byte
                a.cmpRI(RAX, 0);
                Label noGc;
                a.jcc(E, noGc);
                bailTo(target);
                a.bind(noGc);
                branchTo(target);
                break;
            }
            default: return false;                    // pass 1 should have caught it
        }
        if (!ok) return false;
        off = next;
    }

    // Falling off the end of the region: resume at `end`.
    a.movAbs(RAX, (uint64_t)end);
    a.jmp(epilogue);
    emitEpilogue();
    return true;
}

} // namespace Jit
} // namespace Lovax

#endif // LOVAX_JIT_COMPILE_HPP
