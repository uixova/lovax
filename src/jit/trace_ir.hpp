#ifndef LOVAX_JIT_TRACE_IR_HPP
#define LOVAX_JIT_TRACE_IR_HPP

// Linear typed SSA IR for the tracing JIT (RFC-028 Stage-5.6a) — the foundation
// the trace recorder emits into and the backend lowers to machine code.
//
// Method studied from LuaJIT `lj_ir.h` (an IRIns is opcode + type + two operand
// refs, packed) — our own implementation, zero dependencies. Design points:
//   * Every instruction DEFINES a value, referenced everywhere by its index in
//     the buffer (a "ref"). SSA: a ref is written once. This makes constant
//     folding / CSE / DCE (Stage-5.6d) trivial index rewrites.
//   * The recorder keeps the interpreter->IR mapping (a shadow `slot[]` of refs)
//     OUTSIDE the IR; the IR itself is just the straight-line value graph plus a
//     LOOP marker and PHI nodes for loop-carried values.
//   * Guards (SLOAD's implicit type guard, GUARD_TYPE, and the comparison ops
//     when used as guards) side-exit to the interpreter; the live-ref set at the
//     guard is captured by a snapshot (Stage-5.6a basic writeback -> 5.6b compact).
//
// Nothing here emits code or runs yet — this is the data model. The recorder
// (trace_record.hpp) and the backend land next, gated behind --jit-trace so the
// default (RA / template / interpreter) is untouched.

#include <cstdint>
#include <cstring>
#include <vector>

namespace Lovax {
namespace Jit {

// The runtime type a trace ref carries — recorded from the ACTUAL value seen
// while tracing, then GUARDED so a later type change side-exits. Kept small:
// the first traces are numeric hot loops (INT/NUM); everything else guards to
// OBJ or forces a side-exit.
enum class TrType : uint8_t { UNK = 0, INT, NUM, NIL, BOOL, OBJ };

enum class IROp : uint8_t {
    // --- constants (value in `imm`: int64, or double bit-pattern) ---
    KINT, KNUM,
    // --- interpreter slot load (imm = slot index); carries a type guard ---
    SLOAD,
    // --- arithmetic; INT or NUM decided by the instruction's type field ---
    ADD, SUB, MUL, DIV, MOD,
    // --- bitwise (INT only) ---
    BAND, BOR, BXOR,
    // --- comparisons; used as guards (side-exit when the recorded result flips) ---
    LT, LE, GT, GE, EQ, NE,
    // --- int <-> num conversion (op1 = source; type = target) ---
    CONV,
    // --- loop machinery ---
    LOOP,       // marks the loop header (the back-edge target)
    PHI,        // loop-carried value: op1 = entry ref, op2 = back-edge ref
    // --- explicit type guard on op1 (side-exit if not `type`) ---
    GUARD_TYPE,
};

using IRRef = uint16_t;

struct IRIns {
    IROp    op;
    TrType  t   = TrType::UNK;   // type of the value this instruction defines
    uint16_t op1 = 0;           // operand ref (or unused)
    uint16_t op2 = 0;           // operand ref (or unused)
    int64_t  imm = 0;           // KINT: int64 · KNUM: double bits · SLOAD: slot
    int16_t  reg = -1;          // backend register (-1 = none / spilled)
    uint8_t  guard = 0;         // 1 = this instruction side-exits on failure

    double numImm() const { double d; std::memcpy(&d, &imm, 8); return d; }
};

// The IR buffer for one trace. The recorder appends via the emit helpers; the
// backend walks `ins` in order. (Folding/CSE hooks arrive with Stage-5.6d.)
class Trace {
public:
    std::vector<IRIns> ins;

    IRRef emit(IROp op, TrType t, IRRef a = 0, IRRef b = 0) {
        ins.push_back(IRIns{op, t, a, b, 0, -1, 0});
        return (IRRef)(ins.size() - 1);
    }
    IRRef kint(int64_t v)  { IRRef r = emit(IROp::KINT, TrType::INT); ins[r].imm = v; return r; }
    IRRef knum(double v)   { IRRef r = emit(IROp::KNUM, TrType::NUM); std::memcpy(&ins[r].imm, &v, 8); return r; }
    IRRef sload(int slot, TrType t) { IRRef r = emit(IROp::SLOAD, t); ins[r].imm = slot; ins[r].guard = 1; return r; }
    // a guard: side-exits when the (comparison) instruction's runtime result
    // differs from what was recorded.
    IRRef guardOp(IROp op, TrType t, IRRef a, IRRef b) {
        IRRef r = emit(op, t, a, b); ins[r].guard = 1; return r;
    }

    size_t size() const { return ins.size(); }
    void clear() { ins.clear(); }
};

} // namespace Jit
} // namespace Lovax

#endif // LOVAX_JIT_TRACE_IR_HPP
