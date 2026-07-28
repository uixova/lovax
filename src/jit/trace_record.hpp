#ifndef LOVAX_JIT_TRACE_RECORD_HPP
#define LOVAX_JIT_TRACE_RECORD_HPP

// Trace region compiler (RFC-028 Stage-5.6a) — the first FLOAT-capable, and the
// first IR-driven, region compiler. It is the register-allocating compiler's
// numeric successor: same proven skeleton (pinned cells in registers, a range/
// type guard on every boundary, a bail that writes the live values back to the
// interpreter's memory and returns a resume offset), extended in two ways the
// method-JIT could not reach:
//
//   * FLOAT values. A value is a float iff it is NOT NaN-box tagged, so the raw
//     64-bit word already IS the double. We carry that word in a GP register
//     (VT_NUM) exactly like an unboxed int (VT_INT), and only bring it into an
//     XMM scratch for the actual SSE op (movq -> addsd/subsd/mulsd -> movq back).
//     Mixed int/float operands coerce the int with cvtsi2sd. A NaN result bails
//     (the interpreter canonicalises NaN on write; raw x86 NaN could land in the
//     tag range), which keeps the compiled path bit-identical to the interpreter.
//
//   * GLOBALS. Top-level numeric loops (mandel, the float kernels) work on
//     globals, which the RA never modelled. A global touched only by GET/SET is
//     PINNED to a register for the whole region (guarded + loaded in the
//     prologue, written back at every exit); a global that is DEFINE_GLOBAL'd
//     (a fresh per-iteration temp) stays in memory and is read/written there.
//
// The RECORDED TYPE of every cell is observed from the ACTUAL live value at the
// moment the region turns hot (the VM hands us the live slot/global arrays), fed
// into the linear IR (trace_ir.hpp) as a typed SLOAD, and then GUARDED in the
// prologue so a later type change side-exits. This is the tracing idea in its
// smallest honest form — runtime type recording, so float type-inference is free
// (no static backward dataflow) — and it is what unblocks the float kernels.
//
// Scope (Stage-5.6a): numeric (int + float) cells only; the opcode subset in
// traceSupported(); no index/call (those are RA / later phases). Anything else,
// any non-numeric cell, or register pressure over the pool -> the compile bails
// (returns no region) and RA / the template compiler take over, unchanged. The
// interpreter stays the oracle: every gate asserts trace == interpreter bit for
// bit. Gated behind --jit-trace so the default pipeline is untouched.

#include <cstdint>
#include <cstddef>
#include <vector>
#include <unordered_map>
#include "compile.hpp"          // JitCtx, Region, JitFn, JIT_* constants, Asm
#include "trace_ir.hpp"         // linear typed SSA IR (the record target)

namespace Lovax {
namespace Jit {

// Opt-in. --jit-trace turns it on; off by default so RA / template / interpreter
// are the untouched pipeline until the tracer is proven across every gate.
inline bool jitTraceEnabled = false;

class RegionCompilerTrace {
public:
    // fixed registers (a numeric region needs no consts base — int consts are
    // materialised with movabs, float consts as their raw double bits — so R15
    // is free for the pool; RBX joins the pool too when the region has no locals)
    static constexpr Reg R_SLOTS   = RBX;
    static constexpr Reg R_SP      = R12;
    static constexpr Reg R_GLOBALS = R13;
    static constexpr Reg R_CTX     = R14;

    static constexpr uint64_t TAG_INT   = JIT_TAGS | ((uint64_t)4 << JIT_TAG_SHIFT);

    enum VT { VT_INT = 0, VT_NUM = 1 };   // unboxed int64 | raw double-bits word

    const Chunk& chunk;
    size_t start, end;
    const Value* rtSlots;        // live locals   (runtime type oracle)
    const Value* rtGlobals;      // live globals
    const unsigned char* rtDefined;
    Asm a;
    Trace ir;                    // the linear typed IR this records into
    std::unordered_map<size_t, Label> labels;
    Label prologueBail, bailTail;
    bool ok = true;

    // A cell is one interpreter storage location kept live across the region.
    struct Cell { bool isGlobal; int idx; int vt; bool pinned; int poolIdx; };
    std::vector<Cell> cells;
    std::unordered_map<int,int> localCell;    // slot  -> cell index
    std::unordered_map<int,int> globalCell;   // gidx  -> cell index

    std::vector<Reg> gpPool;      // pinnable GP registers (built per region)
    int numPinned = 0;
    int maxDepth = 0;
    std::vector<int> otype;       // operand type by depth (valid for d < depth)

    RegionCompilerTrace(const Chunk& c, size_t s, size_t e,
                        const Value* sl, const Value* gl, const unsigned char* def)
        : chunk(c), start(s), end(e), rtSlots(sl), rtGlobals(gl), rtDefined(def),
          otype(16, VT_INT) {}

    Label& labelAt(size_t off) { return labels[off]; }
    uint16_t rdU16(size_t off) const {
        return (uint16_t)((chunk.code[off] << 8) | chunk.code[off + 1]);
    }
    Reg poolReg(int i)  { return gpPool[i]; }
    Reg pinReg(int cellIdx) { return gpPool[cells[cellIdx].poolIdx]; }
    Reg opReg(int depth) { return gpPool[numPinned + depth]; }

    // ---- boxing / guards (shared shape with the RA compiler) ----
    void boxTo(Reg dst, Reg src) {                 // known-in-range int -> NaN-box word
        a.movRR(dst, src);
        a.movAbs(RCX, JIT_PAYLOAD); a.andRR(dst, RCX);
        a.movAbs(RCX, TAG_INT);     a.orRR(dst, RCX);
    }
    void bailTo(size_t bytecodeOff, int depth) {
        for (int i = 0; i < depth; ++i) {
            if (otype[i] == VT_INT) boxTo(RDX, opReg(i));
            else                    a.movRR(RDX, opReg(i));   // float word is the value
            a.movMR(R_SP, i * 8, RDX);
        }
        if (depth > 0) a.addRI(R_SP, depth * 8);
        a.movAbs(RAX, (uint64_t)bytecodeOff);
        a.jmp(bailTail);
    }
    void rangeCheck(size_t off, int depth) {       // RAX must round-trip through 47 bits
        a.movRR(RCX, RAX); a.shlRI(RCX, 17); a.sarRI(RCX, 17);
        a.cmpRR(RCX, RAX);
        Label okL; a.jcc(E, okL);
        bailTo(off, depth);
        a.bind(okL);
    }
    // guard the word in `r` is an inline int, then unbox it in place
    void guardUnboxInt(Reg r, Label& bail) {
        a.movRR(RCX, r); a.shrRI(RCX, JIT_TAG_SHIFT);
        a.cmpRI(RCX, (int32_t)JIT_TOP17_INT);
        Label okL; a.jcc(E, okL); a.jmp(bail); a.bind(okL);
        a.shlRI(r, 17); a.sarRI(r, 17);
    }
    // guard the word in `r` is a float (NOT NaN-box tagged): (r & TAGS) != TAGS
    void guardFloat(Reg r, Label& bail) {
        a.movRR(RCX, r); a.movAbs(RDX, JIT_TAGS); a.andRR(RCX, RDX); a.cmpRR(RCX, RDX);
        Label okL; a.jcc(NE, okL); a.jmp(bail); a.bind(okL);
    }
    // coerce operand d to an unboxed int (int already; there is no float->int in
    // Stage-5.6a's op set, so a VT_NUM here is a mis-typed program -> bail-compile)
    void ensureInt(int d, size_t off, int depth) {
        if (otype[d] == VT_INT) return;
        ok = false; (void)off; (void)depth;      // caller checks ok
    }
    // bring operand d into XMM x as a double (coerce int -> double)
    void toXmm(int d, Xmm x) {
        if (otype[d] == VT_INT) a.cvtsi2sd(x, opReg(d));
        else                    a.movqXR(x, opReg(d));
    }

    bool compile();
};

inline bool traceSupported(Op op) {
    switch (op) {
        case Op::GET_LOCAL: case Op::SET_LOCAL: case Op::CONST:
        case Op::GET_GLOBAL: case Op::SET_GLOBAL: case Op::DEFINE_GLOBAL:
        case Op::POP: case Op::DUP:
        case Op::ADD: case Op::SUB: case Op::MUL: case Op::ADD_INPLACE:
        case Op::ADD_I: case Op::SUB_I: case Op::MUL_I: case Op::MOD_I:
        case Op::BAND_I: case Op::BOR_I: case Op::BXOR_I:
        case Op::BIT_AND: case Op::BIT_OR: case Op::BIT_XOR:
        case Op::LESS_JF: case Op::LESS_EQ_JF: case Op::GREATER_JF:
        case Op::GREATER_EQ_JF: case Op::EQUAL_JF: case Op::NOT_EQUAL_JF:
        case Op::JUMP: case Op::LOOP:
            return true;
        default:
            return false;
    }
}

inline int traceStackDelta(Op op) {
    switch (op) {
        case Op::GET_LOCAL: case Op::CONST: case Op::DUP: case Op::GET_GLOBAL: return +1;
        case Op::SET_LOCAL: case Op::POP: case Op::SET_GLOBAL: case Op::DEFINE_GLOBAL: return -1;
        case Op::ADD: case Op::SUB: case Op::MUL: case Op::ADD_INPLACE:
        case Op::BIT_AND: case Op::BIT_OR: case Op::BIT_XOR: return -1;
        case Op::ADD_I: case Op::SUB_I: case Op::MUL_I: case Op::MOD_I:
        case Op::BAND_I: case Op::BOR_I: case Op::BXOR_I: return 0;
        case Op::LESS_JF: case Op::LESS_EQ_JF: case Op::GREATER_JF:
        case Op::GREATER_EQ_JF: case Op::EQUAL_JF: case Op::NOT_EQUAL_JF: return -2;
        default: return 0;
    }
}

inline bool RegionCompilerTrace::compile() {
    // ---- pass 1: eligibility, cell discovery, runtime-type classification ----
    // A global that is DEFINE_GLOBAL'd anywhere is a per-iteration temp -> keep it
    // in memory; a global touched only by GET/SET is pinned to a register.
    std::unordered_map<int,bool> defd;   // gidx -> is DEFINE_GLOBAL'd in region
    std::vector<int> localOrder, globalOrder;

    auto trTypeOf = [](VKind k, int& vt) -> bool {
        if (k == VKind::INT)   { vt = RegionCompilerTrace::VT_INT; return true; }
        if (k == VKind::FLOAT) { vt = RegionCompilerTrace::VT_NUM; return true; }
        return false;                     // nil / bool / obj -> not numeric
    };

    int depth = 0;
    for (size_t off = start; off < end; ) {
        Op op = (Op)chunk.code[off];
        int len = instrLength(chunk, off);
        if (len == 0 || !traceSupported(op)) return false;
        if (off + len > end) return false;
        if (op == Op::CONST) {
            const Value& k = chunk.consts[rdU16(off + 1)];
            if (k.tag() == VKind::INT) {
                long long v = k.asInt();
                if (v < -(1ll << 46) || v >= (1ll << 46)) return false;
            } else if (k.tag() != VKind::FLOAT) {
                return false;
            }
        }
        if (op == Op::GET_LOCAL || op == Op::SET_LOCAL) {
            int slot = (int)rdU16(off + 1);
            if (!localCell.count(slot)) { localCell[slot] = -1; localOrder.push_back(slot); }
        }
        if (op == Op::GET_GLOBAL || op == Op::SET_GLOBAL || op == Op::DEFINE_GLOBAL) {
            int g = (int)rdU16(off + 1);
            if (!globalCell.count(g)) { globalCell[g] = -1; globalOrder.push_back(g); }
            if (op == Op::DEFINE_GLOBAL) defd[g] = true;
        }
        depth += traceStackDelta(op);
        if (depth < 0) return false;
        if (depth > maxDepth) maxDepth = depth;
        off += len;
    }

    // Build cells with their runtime-observed types.
    auto addCell = [&](bool isGlobal, int idx, int pinned) -> bool {
        VKind k = isGlobal ? rtGlobals[idx].tag() : rtSlots[idx].tag();
        if (isGlobal && !rtDefined[idx]) return false;     // must be live at entry
        int vt; if (!trTypeOf(k, vt)) return false;
        int ci = (int)cells.size();
        cells.push_back(Cell{isGlobal, idx, vt, (bool)pinned, -1});
        if (isGlobal) globalCell[idx] = ci; else localCell[idx] = ci;
        return true;
    };
    for (int slot : localOrder)  if (!addCell(false, slot, 1)) return false;
    for (int g : globalOrder)    if (!addCell(true, g, defd.count(g) ? 0 : 1)) return false;

    // ---- register file: pin cells, then depth-indexed operands ----
    // No locals -> RBX is free to join the pool (a globals-only region needs no
    // slots base). R15 is always free (no consts base). Fixed: R12/R13/R14.
    bool haveLocals = !localOrder.empty();
    gpPool = { RSI, RDI, RBP, R8, R9, R10, R11, R15 };
    if (!haveLocals) gpPool.push_back(RBX);

    int poolIdx = 0;
    for (auto& c : cells) if (c.pinned) {
        if (poolIdx >= (int)gpPool.size()) return false;
        c.poolIdx = poolIdx++;
    }
    numPinned = poolIdx;
    if (numPinned + maxDepth > (int)gpPool.size()) return false;
    if (numPinned + maxDepth > (int)otype.size()) otype.assign(numPinned + maxDepth + 1, VT_INT);

    // Record the pinned cells into the IR as typed SLOADs (the value graph's
    // entry points; the recorder keeps the cell->ref map outside the IR).
    for (auto& c : cells)
        ir.sload(c.isGlobal ? (1000 + c.idx) : c.idx,
                 c.vt == VT_NUM ? TrType::NUM : TrType::INT);

    // ---- prologue: save regs, load ctx, guard + load pinned cells ----
    a.pushR(RBX); a.pushR(RBP); a.pushR(R12); a.pushR(R13); a.pushR(R14); a.pushR(R15);
    a.movRR(R_CTX, RDI);
    if (haveLocals) a.movRM(R_SLOTS, R_CTX, offsetof(JitCtx, slots));
    a.movRM(R_SP,      R_CTX, offsetof(JitCtx, sp));
    a.movRM(R_GLOBALS, R_CTX, offsetof(JitCtx, globals));

    // Guard every DEFINE'd (memory) global is defined + the recorded type at
    // entry; pinned cells are guarded as they load.
    for (auto& c : cells) {
        if (c.isGlobal) {
            // globalDefined[idx] != 0
            a.movRM(RCX, R_CTX, offsetof(JitCtx, globalDefined));
            a.movzxRM8(RAX, RCX, c.idx);
            a.cmpRI(RAX, 0);
            Label okD; a.jcc(NE, okD); a.jmp(prologueBail); a.bind(okD);
        }
        if (c.pinned) {
            Reg r = pinReg((int)(&c - &cells[0]));
            if (c.isGlobal) a.movRM(r, R_GLOBALS, c.idx * 8);
            else            a.movRM(r, R_SLOTS,   c.idx * 8);
            if (c.vt == VT_INT) guardUnboxInt(r, prologueBail);
            else                guardFloat(r, prologueBail);
        } else {
            // memory global: guard its current type matches the recorded type
            a.movRM(RAX, R_GLOBALS, c.idx * 8);
            if (c.vt == VT_INT) guardUnboxInt(RAX, prologueBail);
            else                guardFloat(RAX, prologueBail);
        }
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
        // numeric binary op on operands (d-2,d-1) -> d-2
        auto numBinary = [&](int kind) {   // 0=add 1=sub 2=mul (int or float)
            int lt = otype[depth - 2], rt = otype[depth - 1];
            Reg lhs = opReg(depth - 2), rhs = opReg(depth - 1);
            if (lt == VT_INT && rt == VT_INT) {
                a.movRR(RAX, lhs);
                if (kind == 0) a.addRR(RAX, rhs);
                else if (kind == 1) a.subRR(RAX, rhs);
                else a.imulRR(RAX, rhs);
                rangeCheck(off, depth);
                a.movRR(lhs, RAX);
                otype[depth - 2] = VT_INT;
            } else {
                toXmm(depth - 2, XMM0);
                toXmm(depth - 1, XMM1);
                if (kind == 0) a.addsd(XMM0, XMM1);
                else if (kind == 1) a.subsd(XMM0, XMM1);
                else a.mulsd(XMM0, XMM1);
                a.ucomisd(XMM0, XMM0);           // NaN result -> bail (canonicalisation)
                Label okN; a.jcc(NP, okN); bailTo(off, depth); a.bind(okN);
                a.movqRX(lhs, XMM0);
                otype[depth - 2] = VT_NUM;
            }
            depth--;
        };

        switch (op) {
            case Op::GET_LOCAL: {
                int ci = localCell[(int)rdU16(off + 1)];
                a.movRR(opReg(depth), pinReg(ci));
                otype[depth] = cells[ci].vt; depth++; break;
            }
            case Op::SET_LOCAL: {
                int ci = localCell[(int)rdU16(off + 1)];
                if (cells[ci].vt == VT_INT) ensureInt(depth - 1, off, depth);
                else if (otype[depth - 1] != VT_NUM) { ok = false; }
                a.movRR(pinReg(ci), opReg(depth - 1));
                depth--; break;
            }
            case Op::GET_GLOBAL: {
                int ci = globalCell[(int)rdU16(off + 1)];
                if (cells[ci].pinned) {
                    a.movRR(opReg(depth), pinReg(ci));   // pinned: already unboxed/raw
                } else {
                    a.movRM(opReg(depth), R_GLOBALS, cells[ci].idx * 8);
                    // memory INT cells are stored boxed (see SET/DEFINE) -> unbox on
                    // load; every in-region write range-checked before boxing, and
                    // the prologue guarded the entry value, so no guard is needed.
                    if (cells[ci].vt == VT_INT) { a.shlRI(opReg(depth), 17); a.sarRI(opReg(depth), 17); }
                }
                otype[depth] = cells[ci].vt; depth++; break;
            }
            case Op::SET_GLOBAL: case Op::DEFINE_GLOBAL: {
                int ci = globalCell[(int)rdU16(off + 1)];
                if (cells[ci].vt == VT_INT) ensureInt(depth - 1, off, depth);
                else if (otype[depth - 1] != VT_NUM) { ok = false; }
                if (cells[ci].pinned) {
                    a.movRR(pinReg(ci), opReg(depth - 1));
                } else {
                    // memory global: store the value word straight to memory
                    if (cells[ci].vt == VT_INT) { boxTo(RDX, opReg(depth - 1)); a.movMR(R_GLOBALS, cells[ci].idx * 8, RDX); }
                    else                        { a.movMR(R_GLOBALS, cells[ci].idx * 8, opReg(depth - 1)); }
                }
                depth--; break;
            }
            case Op::CONST: {
                const Value& k = chunk.consts[rdU16(off + 1)];
                if (k.tag() == VKind::INT) {
                    a.movAbs(opReg(depth), (uint64_t)k.asInt());
                    otype[depth] = VT_INT;
                } else {
                    uint64_t bits; double d = k.asFloat(); std::memcpy(&bits, &d, 8);
                    a.movAbs(opReg(depth), bits);
                    otype[depth] = VT_NUM;
                }
                depth++; break;
            }
            case Op::POP: depth--; break;
            case Op::DUP: a.movRR(opReg(depth), opReg(depth - 1)); otype[depth] = otype[depth - 1]; depth++; break;

            case Op::ADD: case Op::ADD_INPLACE: numBinary(0); break;
            case Op::SUB: numBinary(1); break;
            case Op::MUL: numBinary(2); break;

            case Op::BIT_AND: case Op::BIT_OR: case Op::BIT_XOR: {
                ensureInt(depth - 2, off, depth); ensureInt(depth - 1, off, depth);
                if (!ok) return false;
                Reg lhs = opReg(depth - 2), rhs = opReg(depth - 1);
                a.movRR(RAX, lhs);
                if (op == Op::BIT_AND) a.andRR(RAX, rhs);
                else if (op == Op::BIT_OR) a.orRR(RAX, rhs);
                else a.xorRR(RAX, rhs);
                a.movRR(lhs, RAX); otype[depth - 2] = VT_INT; depth--; break;
            }

            case Op::ADD_I: case Op::SUB_I: case Op::MUL_I:
            case Op::BAND_I: case Op::BOR_I: case Op::BXOR_I: {
                ensureInt(depth - 1, off, depth);
                if (!ok) return false;
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
                if (op == Op::ADD_I || op == Op::SUB_I || op == Op::MUL_I) rangeCheck(off, depth);
                a.movRR(r, RAX); break;
            }
            case Op::MOD_I: {
                ensureInt(depth - 1, off, depth);
                if (!ok) return false;
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
                uint16_t d = rdU16(off + 1);
                size_t target = next + d;
                int lt = otype[depth - 2], rt = otype[depth - 1];
                Reg lhs = opReg(depth - 2), rhs = opReg(depth - 1);
                if (lt == VT_INT && rt == VT_INT) {
                    depth -= 2;
                    a.cmpRR(lhs, rhs);
                    Cond keep = op == Op::LESS_JF ? L : op == Op::LESS_EQ_JF ? LE
                              : op == Op::GREATER_JF ? G : op == Op::GREATER_EQ_JF ? GE
                              : op == Op::EQUAL_JF ? E : NE;
                    Label fall; a.jcc(keep, fall); branchTo(target); a.bind(fall);
                } else {
                    toXmm(depth - 2, XMM0); toXmm(depth - 1, XMM1);
                    a.ucomisd(XMM0, XMM1);
                    Label okN; a.jcc(NP, okN); bailTo(off, depth); a.bind(okN);
                    depth -= 2;
                    Cond keep = op == Op::LESS_JF ? B : op == Op::LESS_EQ_JF ? BE
                              : op == Op::GREATER_JF ? A : op == Op::GREATER_EQ_JF ? AE
                              : op == Op::EQUAL_JF ? E : NE;
                    Label fall; a.jcc(keep, fall); branchTo(target); a.bind(fall);
                }
                break;
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

    // ---- normal exit + shared writeback tail ----
    a.movAbs(RAX, (uint64_t)end);
    a.jmp(bailTail);

    a.bind(bailTail);
    for (auto& c : cells) {
        if (!c.pinned) continue;                  // memory cells already coherent
        Reg r = pinReg((int)(&c - &cells[0]));
        if (c.vt == VT_INT) boxTo(RDX, r);
        else                a.movRR(RDX, r);
        if (c.isGlobal) a.movMR(R_GLOBALS, c.idx * 8, RDX);
        else            a.movMR(R_SLOTS,   c.idx * 8, RDX);
    }
    a.movMR(R_CTX, offsetof(JitCtx, sp), R_SP);
    a.popR(R15); a.popR(R14); a.popR(R13); a.popR(R12); a.popR(RBP); a.popR(RBX);
    a.ret();

    // ---- prologue bail: nothing in memory touched; resume at `start` ----
    a.bind(prologueBail);
    a.movMR(R_CTX, offsetof(JitCtx, sp), R_SP);
    a.movAbs(RAX, (uint64_t)start);
    a.popR(R15); a.popR(R14); a.popR(R13); a.popR(R12); a.popR(RBP); a.popR(RBX);
    a.ret();
    return ok;
}

inline Region compileRegionTrace(const Chunk& c, size_t start, size_t end,
                                 const Value* slots, const Value* globals,
                                 const unsigned char* defined) {
    Region r;
    RegionCompilerTrace rc(c, start, end, slots, globals, defined);
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

#endif // LOVAX_JIT_TRACE_RECORD_HPP
