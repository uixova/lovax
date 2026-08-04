#ifndef LOVAX_JIT_TRACE_RECORD_HPP
#define LOVAX_JIT_TRACE_RECORD_HPP

// Trace region compiler (RFC-028 Stage-5.6a..d) — the FLOAT-capable, IR-driven
// region compiler. It is the register allocator's numeric successor, extended in
// four steps the method-JIT could not reach:
//
//   5.6a  FLOAT values + GLOBALS. Types are observed from the live slot/global
//         value the moment the loop turns hot (runtime type recording), fed into
//         the linear typed IR (trace_ir.hpp) as typed SLOADs and guarded in the
//         prologue so a later type change side-exits.
//   5.6b  A compact deopt snapshot: read-only pinned cells are not written back
//         at exits (their memory is already current — dirty tracking).
//   5.6c  Call inlining: a leaf callee's body is spliced into a hot loop, its
//         params mapped to the argument registers, the callee global guarded.
//   5.6d  A proper register file: INT values live UNBOXED in GP registers, FLOAT
//         values live as DOUBLES in XMM registers across the whole region — so a
//         float travelling through a chain of ops pays no per-op GP<->XMM move.
//         Only a genuine int<->float coercion (cvtsi2sd) and the memory boundary
//         touch both files. This is the win LuaJIT gets from keeping floats in
//         xmm; a float value is its raw NaN-box word, so a load/store is a plain
//         movsd of the 8-byte double.
//
// A NaN float result bails (the interpreter canonicalises NaN on write), keeping
// the compiled path bit-identical to the interpreter. Scope: numeric (int+float)
// cells, the opcode subset in traceSupported() + inlined leaf calls; no index (RA
// handles those). Anything else, any non-numeric cell, or register pressure over
// a pool makes the compile bail and RA / the template compiler take over. The
// interpreter stays the oracle. Gated behind --jit-trace.

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include "compile.hpp"          // JitCtx, Region, JitFn, JIT_* constants, Asm
#include "trace_ir.hpp"         // linear typed SSA IR (the record target)

namespace Lovax {
namespace Jit {

// On by default now that the tracer is proven bit-identical to the interpreter
// across golden, the differential --jit-trace axis (122 programs), an 800-program
// numeric + call-inline fuzz, and ASan+UBSan+GC_STRESS_INC. It sits AFTER the RA
// in the pipeline (RA keeps the pure-int-local loops it is optimal at), taking the
// float / global loops the RA declines — so it never regresses what RA does and
// delivers the float-kernel win out of the box. --no-trace turns it off.
inline bool jitTraceEnabled = true;

class RegionCompilerTrace {
public:
    static constexpr Reg R_SLOTS   = RBX;
    static constexpr Reg R_SP      = R12;
    static constexpr Reg R_GLOBALS = R13;
    static constexpr Reg R_CTX     = R14;

    static constexpr uint64_t TAG_INT   = JIT_TAGS | ((uint64_t)4 << JIT_TAG_SHIFT);
    // list indexing (ported from the RA compiler; verified layout)
    static constexpr uint32_t TOP17_OBJ    = 0x1FFF5u;
    static constexpr uint32_t TOP17_BOXINT = 0x1FFF6u;
    static constexpr int32_t  OBJ_TAG_OFF   = 8;
    static constexpr int32_t  VEC_START_OFF = 32;    // offsetof(ListObject,elements)
    static constexpr int32_t  VEC_FINISH_OFF= 40;
    static constexpr int      LIST_TAG      = 5;     // ObjectType::LIST
    // struct instance (Stage-8 G2): shape ptr + inline-Value slots (SmallVec begin).
    static constexpr int32_t  STRUCT_SHAPE_OFF = 32; // offsetof(StructInstanceObject, shape)
    static constexpr int32_t  STRUCT_SLOTS_OFF = 40; // offsetof(slots) + SmallVec begin(0)
    static constexpr int      STRUCT_TAG       = 17; // ObjectType::STRUCT

    // unboxed int64 (GP) | raw double in an XMM register | a callee closure word
    // in GP | a pinned list-object raw word (index base, GP) | a raw non-numeric
    // constant word e.g. a bool (GP, storable into a list, never arithmetic) |
    // a pinned struct-instance raw word (member base, GP, prologue-guarded shape).
    enum VT { VT_INT = 0, VT_NUM = 1, VT_CALLEE = 2, VT_OBJ = 3, VT_WORD = 4, VT_STRUCT = 5 };

    const Chunk& chunk;
    size_t start, end;
    const Value* rtSlots;
    const Value* rtGlobals;
    const unsigned char* rtDefined;
    Asm a;
    Trace ir;
    std::unordered_map<size_t, Label> labels;
    Label prologueBail, bailTail;
    bool ok = true;

    // A cell is one interpreter storage location kept live across the region.
    // poolIdx indexes the GP pool for an INT/CALLEE cell, the XMM pool for a NUM
    // cell. `dirty` = written in the region (a read-only pinned cell needs no
    // writeback — its memory already holds the value).
    struct Cell { bool isGlobal; int idx; int vt; bool pinned; bool dirty; int poolIdx; const void* shape = nullptr; };
    std::vector<Cell> cells;
    std::unordered_map<int,int> localCell;
    std::unordered_map<int,int> globalCell;
    // Stage-8 G2 struct member access, resolved per MEMBER op at compile time.
    std::unordered_set<int> structBaseG, structBaseL;         // cell used as a member base
    std::unordered_map<int, const void*> gShape, lShape;      // struct-base cell -> shape ptr
    std::unordered_map<size_t, int> memberSlot;               // MEMBER off -> field slot
    std::unordered_map<size_t, int> memberFType;              // MEMBER off -> field VT (INT/NUM)

    std::vector<Reg> gpPool;      // INT/CALLEE cells (pinned) then INT/CALLEE operands
    std::vector<Xmm> xmmPool;     // NUM cells (pinned) then NUM operands
    int numGpPinned = 0, numXmmPinned = 0;
    int maxDepth = 0;
    std::vector<int> otype;       // operand type by depth

    struct Snapshot { size_t resume; int depth; };
    std::vector<Snapshot> snaps;

    std::unordered_set<int> calleeGlobals;
    std::unordered_map<size_t, const Proto*> inlineProto;
    std::unordered_map<size_t, uint64_t>     calleeWord;
    std::unordered_map<size_t, int>          calleeArgc;
    std::unordered_map<size_t, int>          intrinsicAt;   // CALL off -> math intrinsic (1=sqrt)

    RegionCompilerTrace(const Chunk& c, size_t s, size_t e,
                        const Value* sl, const Value* gl, const unsigned char* def)
        : chunk(c), start(s), end(e), rtSlots(sl), rtGlobals(gl), rtDefined(def),
          otype(16, VT_INT) {}

    Label& labelAt(size_t off) { return labels[off]; }
    uint16_t rdU16(size_t off) const {
        return (uint16_t)((chunk.code[off] << 8) | chunk.code[off + 1]);
    }
    Reg gp(int d)      { return gpPool[numGpPinned + d]; }
    Xmm xm(int d)      { return xmmPool[numXmmPinned + d]; }
    Reg gpCell(int ci) { return gpPool[cells[ci].poolIdx]; }
    Xmm xmCell(int ci) { return xmmPool[cells[ci].poolIdx]; }

    // ---- boxing / guards ----
    void boxTo(Reg dst, Reg src) {                 // known-in-range int -> NaN-box word
        a.movRR(dst, src);
        a.movAbs(RCX, JIT_PAYLOAD); a.andRR(dst, RCX);
        a.movAbs(RCX, TAG_INT);     a.orRR(dst, RCX);
    }
    // publish live operands to the memory stack and side-exit to `bytecodeOff`.
    void bailTo(size_t bytecodeOff, int depth) {
        snaps.push_back(Snapshot{bytecodeOff, depth});
        for (int i = 0; i < depth; ++i) {
            if (otype[i] == VT_NUM) {
                a.movsdMX(R_SP, i * 8, xm(i));         // the double IS the value word
            } else if (otype[i] == VT_INT) {
                boxTo(RDX, gp(i)); a.movMR(R_SP, i * 8, RDX);
            } else {                                   // VT_CALLEE: raw closure word
                a.movRR(RDX, gp(i)); a.movMR(R_SP, i * 8, RDX);
            }
        }
        if (depth > 0) a.addRI(R_SP, depth * 8);
        a.movAbs(RAX, (uint64_t)bytecodeOff);
        a.jmp(bailTail);
    }
    void guardUnboxInt(Reg r, Label& bail) {
        a.movRR(RCX, r); a.shrRI(RCX, JIT_TAG_SHIFT);
        a.cmpRI(RCX, (int32_t)JIT_TOP17_INT);
        Label okL; a.jcc(E, okL); a.jmp(bail); a.bind(okL);
        a.shlRI(r, 17); a.sarRI(r, 17);
    }
    void guardFloat(Reg r, Label& bail) {          // (r & TAGS) != TAGS  -> a float
        a.movRR(RCX, r); a.movAbs(RDX, JIT_TAGS); a.andRR(RCX, RDX); a.cmpRR(RCX, RDX);
        Label okL; a.jcc(NE, okL); a.jmp(bail); a.bind(okL);
    }
    void ensureInt(int d) { if (otype[d] != VT_INT) ok = false; }   // no float->int op
    // &list[idx] -> RAX from a raw list word `objR` and unboxed int `idxR`; bails
    // out-of-range (incl. negative). `trusted` skips the object + LIST-tag guard —
    // valid because every VT_OBJ operand originates from a pinned list cell already
    // guarded as an object AND a LIST in the prologue, and such a cell is read-only
    // for the whole region, so only the per-access BOUNDS check is needed (this is
    // the guard-hoisting LICM does for list kernels). Clobbers RAX/RCX/RDX.
    void indexAddr(Reg objR, Reg idxR, size_t off, int depth, bool trusted) {
        if (!trusted) {
            a.movRR(RCX, objR); a.shrRI(RCX, JIT_TAG_SHIFT);
            a.cmpRI(RCX, (int32_t)TOP17_OBJ);
            Label o1; a.jcc(E, o1); bailTo(off, depth); a.bind(o1);
            a.movAbs(RCX, JIT_PAYLOAD); a.movRR(RAX, objR); a.andRR(RAX, RCX);   // RAX = Object*
            a.movzxRM8(RCX, RAX, OBJ_TAG_OFF);
            a.cmpRI(RCX, LIST_TAG);
            Label o2; a.jcc(E, o2); bailTo(off, depth); a.bind(o2);
        } else {
            a.movAbs(RCX, JIT_PAYLOAD); a.movRR(RAX, objR); a.andRR(RAX, RCX);   // RAX = Object* (guaranteed list)
        }
        a.movRM(RDX, RAX, VEC_START_OFF);
        a.movRM(RCX, RAX, VEC_FINISH_OFF); a.subRR(RCX, RDX);                // RCX = size*8
        a.movRR(RAX, idxR); a.shlRI(RAX, 3);
        a.cmpRR(RAX, RCX);
        Label o3; a.jcc(B, o3); bailTo(off, depth); a.bind(o3);              // unsigned: catches <0
        a.addRR(RAX, RDX);                                                   // RAX = &list[idx]
    }
    // ensure operand d is a double in xm(d), converting an unboxed int in place.
    void toX(int d) {
        if (otype[d] == VT_NUM) return;
        a.cvtsi2sd(xm(d), gp(d));
        otype[d] = VT_NUM;
    }

    // ---- numeric primitives shared by the main body and inlined callees ----
    bool numOperand(int d) { return otype[d] == VT_INT || otype[d] == VT_NUM; }
    void numericBinary(int kind, int dd, size_t bailOff, int bailDepth) {  // 0=add 1=sub 2=mul
        if (!numOperand(dd - 2) || !numOperand(dd - 1)) { ok = false; return; }
        if (otype[dd - 2] == VT_INT && otype[dd - 1] == VT_INT) {
            Reg lhs = gp(dd - 2), rhs = gp(dd - 1);
            a.movRR(RAX, lhs);
            if (kind == 0) a.addRR(RAX, rhs);
            else if (kind == 1) a.subRR(RAX, rhs);
            else a.imulRR(RAX, rhs);
            a.movRR(RCX, RAX); a.shlRI(RCX, 17); a.sarRI(RCX, 17); a.cmpRR(RCX, RAX);
            Label okL; a.jcc(E, okL); bailTo(bailOff, bailDepth); a.bind(okL);
            a.movRR(lhs, RAX);
            otype[dd - 2] = VT_INT;
        } else {
            toX(dd - 2); toX(dd - 1);
            if (kind == 0) a.addsd(xm(dd - 2), xm(dd - 1));
            else if (kind == 1) a.subsd(xm(dd - 2), xm(dd - 1));
            else a.mulsd(xm(dd - 2), xm(dd - 1));
            a.ucomisd(xm(dd - 2), xm(dd - 2));
            Label okN; a.jcc(NP, okN); bailTo(bailOff, bailDepth); a.bind(okN);
            otype[dd - 2] = VT_NUM;
        }
    }
    void immBinary(Op op, int16_t k, int dd, size_t bailOff, int bailDepth) {
        if (otype[dd - 1] != VT_INT) { ok = false; return; }
        Reg r = gp(dd - 1);
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
        if (op == Op::ADD_I || op == Op::SUB_I || op == Op::MUL_I) {
            a.movRR(RCX, RAX); a.shlRI(RCX, 17); a.sarRI(RCX, 17); a.cmpRR(RCX, RAX);
            Label okL; a.jcc(E, okL); bailTo(bailOff, bailDepth); a.bind(okL);
        }
        a.movRR(r, RAX);
    }
    void modBinary(int16_t k, int dd) {
        Reg r = gp(dd - 1);
        a.movRR(RAX, r); a.movAbs(RCX, (uint64_t)(int64_t)k);
        a.cqo(); a.idivR(RCX); a.movRR(RAX, RDX);
        Label done;
        a.cmpRI(RAX, 0); a.jcc(E, done);
        a.movRR(RDX, RAX); a.xorRR(RDX, RCX);
        a.cmpRI(RDX, 0); a.jcc(GE, done);
        a.addRR(RAX, RCX);
        a.bind(done);
        a.movRR(r, RAX);
    }
    void bitBinary(Op op, int dd) {
        if (otype[dd - 2] != VT_INT || otype[dd - 1] != VT_INT) { ok = false; return; }
        Reg lhs = gp(dd - 2), rhs = gp(dd - 1);
        a.movRR(RAX, lhs);
        if (op == Op::BIT_AND) a.andRR(RAX, rhs);
        else if (op == Op::BIT_OR) a.orRR(RAX, rhs);
        else a.xorRR(RAX, rhs);
        a.movRR(lhs, RAX); otype[dd - 2] = VT_INT;
    }

    bool leafOk(const Proto* p, int argc, int& calleeMaxDepth) {
        if (p->variadic || p->upvalueCount != 0) return false;
        if (p->paramCount != argc || p->requiredCount != argc) return false;
        if (p->localCount != p->paramCount) return false;
        const Chunk& cc = p->chunk;
        int dd = 0; calleeMaxDepth = 0;
        for (size_t o = 0; o < cc.code.size(); ) {
            Op op = (Op)cc.code[o];
            int len = instrLength(cc, o);
            if (len == 0) return false;
            switch (op) {
                case Op::GET_LOCAL: dd += 1; break;
                case Op::LGET2:     dd += 2; break;
                case Op::CONST: {
                    const Value& k = cc.consts[(uint16_t)((cc.code[o+1]<<8)|cc.code[o+2])];
                    if (k.tag() == VKind::INT) { long long v=k.asInt(); if (v < -(1ll<<46) || v >= (1ll<<46)) return false; }
                    else if (k.tag() != VKind::FLOAT) return false;
                    dd += 1; break;
                }
                case Op::ADD: case Op::SUB: case Op::MUL: case Op::ADD_INPLACE:
                case Op::BIT_AND: case Op::BIT_OR: case Op::BIT_XOR: dd -= 1; break;
                case Op::ADD_I: case Op::SUB_I: case Op::MUL_I: case Op::MOD_I:
                case Op::BAND_I: case Op::BOR_I: case Op::BXOR_I:
                    if (op == Op::MOD_I && (int16_t)((cc.code[o+1]<<8)|cc.code[o+2]) == 0) return false;
                    break;
                case Op::RETURN:
                    return dd == 1;
                default: return false;
            }
            if (dd < 0) return false;
            if (dd > calleeMaxDepth) calleeMaxDepth = dd;
            o += len;
        }
        return false;
    }
    // Emit the leaf callee inline. Args occupy slots base-argc..base-1; callee
    // local i == arg i. Temps push above `base`. Result lands at base-argc-1.
    void emitInlineCallee(const Proto* p, int base, int argc, size_t callOff) {
        const Chunk& cc = p->chunk;
        int argBase = base - argc;
        int dd = base;
        auto pushLocal = [&](int i) {
            int s = argBase + i;
            if (otype[s] == VT_NUM) a.movsdRR(xm(dd), xm(s));
            else                    a.movRR(gp(dd), gp(s));
            otype[dd] = otype[s]; dd++;
        };
        for (size_t o = 0; o < cc.code.size(); ) {
            Op op = (Op)cc.code[o];
            int len = instrLength(cc, o);
            uint16_t a0 = (uint16_t)((cc.code[o+1]<<8)|cc.code[o+2]);
            switch (op) {
                case Op::GET_LOCAL: pushLocal((int)a0); break;
                case Op::LGET2: pushLocal((int)a0); pushLocal((int)(uint16_t)((cc.code[o+3]<<8)|cc.code[o+4])); break;
                case Op::CONST: {
                    const Value& k = cc.consts[a0];
                    if (k.tag() == VKind::INT) { a.movAbs(gp(dd), (uint64_t)k.asInt()); otype[dd] = VT_INT; }
                    else { uint64_t b; double d = k.asFloat(); std::memcpy(&b,&d,8); a.movAbs(RAX, b); a.movqXR(xm(dd), RAX); otype[dd] = VT_NUM; }
                    dd++; break;
                }
                case Op::ADD: case Op::ADD_INPLACE: numericBinary(0, dd, callOff, base); dd--; break;
                case Op::SUB: numericBinary(1, dd, callOff, base); dd--; break;
                case Op::MUL: numericBinary(2, dd, callOff, base); dd--; break;
                case Op::BIT_AND: case Op::BIT_OR: case Op::BIT_XOR: bitBinary(op, dd); dd--; break;
                case Op::ADD_I: case Op::SUB_I: case Op::MUL_I:
                case Op::BAND_I: case Op::BOR_I: case Op::BXOR_I:
                    immBinary(op, (int16_t)a0, dd, callOff, base); break;
                case Op::MOD_I: if (otype[dd-1] != VT_INT) { ok = false; } else modBinary((int16_t)a0, dd); break;
                case Op::RETURN: {
                    int r = base - argc - 1;
                    if (otype[dd - 1] == VT_NUM) a.movsdRR(xm(r), xm(dd - 1));
                    else                         a.movRR(gp(r), gp(dd - 1));
                    otype[r] = otype[dd - 1];
                    return;
                }
                default: ok = false; return;
            }
            if (!ok) return;
            o += len;
        }
        ok = false;
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
        case Op::LT_I_JF: case Op::LE_I_JF: case Op::GT_I_JF:
        case Op::GE_I_JF: case Op::EQ_I_JF: case Op::NE_I_JF:
        case Op::CALL:
        case Op::INDEX_SET: case Op::INDEX_GET: case Op::LGET2:
        case Op::MEMBER_GET: case Op::MEMBER_SET:
        case Op::FALSE_: case Op::TRUE_: case Op::NIL: case Op::JUMP_IF_FALSE:
        case Op::JUMP: case Op::LOOP:
            return true;
        default:
            return false;
    }
}

inline int traceStackDelta(Op op) {
    switch (op) {
        case Op::GET_LOCAL: case Op::CONST: case Op::DUP: case Op::GET_GLOBAL:
        case Op::FALSE_: case Op::TRUE_: case Op::NIL: return +1;
        case Op::SET_LOCAL: case Op::POP: case Op::SET_GLOBAL: case Op::DEFINE_GLOBAL:
        case Op::JUMP_IF_FALSE: case Op::INDEX_GET: return -1;
        case Op::INDEX_SET: return -3;
        case Op::LGET2: return +2;
        case Op::MEMBER_GET: return 0;    // [obj] -> [field]
        case Op::MEMBER_SET: return -2;   // [obj, val] -> []
        case Op::ADD: case Op::SUB: case Op::MUL: case Op::ADD_INPLACE:
        case Op::BIT_AND: case Op::BIT_OR: case Op::BIT_XOR: return -1;
        case Op::ADD_I: case Op::SUB_I: case Op::MUL_I: case Op::MOD_I:
        case Op::BAND_I: case Op::BOR_I: case Op::BXOR_I: return 0;
        case Op::LESS_JF: case Op::LESS_EQ_JF: case Op::GREATER_JF:
        case Op::GREATER_EQ_JF: case Op::EQUAL_JF: case Op::NOT_EQUAL_JF: return -2;
        case Op::LT_I_JF: case Op::LE_I_JF: case Op::GT_I_JF:
        case Op::GE_I_JF: case Op::EQ_I_JF: case Op::NE_I_JF: return -1;
        default: return 0;
    }
}

inline bool RegionCompilerTrace::compile() {
    // ---- pass 1: eligibility, cell discovery, runtime-type classification ----
    std::unordered_map<int,bool> defd;
    std::unordered_map<int,bool> dirtyL, dirtyG;
    std::unordered_map<int,bool> objBaseG, objBaseL;   // used as an INDEX_SET base
    std::vector<int> localOrder, globalOrder;

    auto trTypeOf = [](VKind k, int& vt) -> bool {
        if (k == VKind::INT)   { vt = RegionCompilerTrace::VT_INT; return true; }
        if (k == VKind::FLOAT) { vt = RegionCompilerTrace::VT_NUM; return true; }
        return false;
    };

    std::vector<int> orig;
    auto opush = [&](int o){ orig.push_back(o); };
    auto opop  = [&](int n){ for (int i=0;i<n && !orig.empty();++i) orig.pop_back(); };
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
            if (op == Op::SET_LOCAL) dirtyL[slot] = true;
        }
        if (op == Op::LGET2) {                          // registers BOTH locals it reads
            for (int w = 0; w < 2; ++w) {
                int slot = (int)rdU16(off + 1 + w * 2);
                if (!localCell.count(slot)) { localCell[slot] = -1; localOrder.push_back(slot); }
            }
        }
        if (op == Op::GET_GLOBAL || op == Op::SET_GLOBAL || op == Op::DEFINE_GLOBAL) {
            int g = (int)rdU16(off + 1);
            if (!globalCell.count(g)) { globalCell[g] = -1; globalOrder.push_back(g); }
            if (op == Op::DEFINE_GLOBAL) defd[g] = true;
            if (op == Op::SET_GLOBAL || op == Op::DEFINE_GLOBAL) dirtyG[g] = true;
        }

        if (op == Op::CALL) {
            int argc = chunk.code[off + 1];
            int calleePos = depth - argc - 1;
            if (calleePos < 0) return false;
            int g = (calleePos < (int)orig.size()) ? orig[calleePos] : -1;
            if (g < 0) return false;
            if (!rtGlobals[g].isObj()) return false;
            Object* ob = rtGlobals[g].asObj();
            // Math intrinsic: sqrt(x) — a native builtin call in a hot float loop is
            // emitted as an SSE instruction instead of declining the region. Guarded
            // that the global still holds this exact builtin at run time.
            if (ob && ob->tag == ObjectType::BUILTIN &&
                static_cast<BuiltinObject*>(ob)->name == "sqrt" && argc == 1) {
                uint64_t w; std::memcpy(&w, &rtGlobals[g], 8);
                intrinsicAt[off] = 1; calleeWord[off] = w; calleeArgc[off] = argc;
                calleeGlobals.insert(g);
                int peak = depth + 1; if (peak > maxDepth) maxDepth = peak;
                opop(argc + 1); opush(-1); depth -= argc;
                off += len; continue;                     // handled; next op
            }
            if (!ob || ob->tag != ObjectType::FUNCTION) return false;
            ClosureObject* clo = static_cast<ClosureObject*>(ob);
            if (clo->moduleGlobals != nullptr || clo->structShape) return false;
            const Proto* p = clo->proto.get();
            int calleeMaxDepth = 0;
            if (!p || !leafOk(p, argc, calleeMaxDepth)) return false;
            uint64_t w; std::memcpy(&w, &rtGlobals[g], 8);
            inlineProto[off] = p; calleeWord[off] = w; calleeArgc[off] = argc;
            calleeGlobals.insert(g);
            int peak = depth + calleeMaxDepth;
            if (peak > maxDepth) maxDepth = peak;
            opop(argc + 1); opush(-1);
            depth -= argc;
        } else {
            // origin encoding: global idx >= 0, local slot as -(2+slot), else -1.
            if (op == Op::INDEX_SET) {
                int b = (int)orig.size() - 3;                 // [.. base idx val]
                if (b < 0) return false;
                int o = orig[b];
                if (o >= 0) objBaseG[o] = true;
                else if (o <= -2) objBaseL[-2 - o] = true;
                else return false;                            // base not a plain cell
            }
            if (op == Op::MEMBER_GET || op == Op::MEMBER_SET) {
                // resolve the member from the LIVE struct instance: field slot +
                // field type + shape (for the prologue monomorphic guard). Numeric
                // fields only; anything else declines the region.
                int b = (op == Op::MEMBER_GET) ? (int)orig.size() - 1 : (int)orig.size() - 2;
                if (b < 0 || b >= (int)orig.size()) return false;
                int o = orig[b];
                const Value* bv = (o >= 0) ? &rtGlobals[o] : (o <= -2 ? &rtSlots[-2 - o] : nullptr);
                if (!bv || !bv->isObjType(ObjectType::STRUCT)) return false;
                auto* si = static_cast<StructInstanceObject*>(bv->asObj());
                const Value& nk = chunk.consts[rdU16(off + 1)];
                if (!nk.isObj()) return false;
                const std::string& fname = static_cast<StringObject*>(nk.asObj())->value;
                auto it = si->shape->fieldIndex.find(fname);
                if (it == si->shape->fieldIndex.end()) return false;
                int slot = it->second;
                if (slot < 0 || slot >= (int)si->slots.size()) return false;
                VKind ft = si->slots[slot].tag();
                int fvt = (ft == VKind::INT) ? VT_INT : (ft == VKind::FLOAT) ? VT_NUM : -1;
                if (fvt < 0) return false;                    // numeric fields only
                memberSlot[off] = slot; memberFType[off] = fvt;
                if (o >= 0) { structBaseG.insert(o); gShape[o] = si->shape; }
                else { int s = -2 - o; structBaseL.insert(s); lShape[s] = si->shape; }
            }
            int d = traceStackDelta(op);
            switch (op) {
                case Op::GET_GLOBAL: opush((int)rdU16(off + 1)); break;
                case Op::GET_LOCAL: opush(-2 - (int)rdU16(off + 1)); break;
                case Op::LGET2: opush(-2 - (int)rdU16(off + 1)); opush(-2 - (int)rdU16(off + 3)); break;
                case Op::CONST: case Op::FALSE_: case Op::TRUE_: case Op::NIL: opush(-1); break;
                case Op::DUP: opush(orig.empty() ? -1 : orig.back()); break;
                case Op::SET_LOCAL: case Op::SET_GLOBAL: case Op::DEFINE_GLOBAL:
                case Op::POP: case Op::JUMP_IF_FALSE: opop(1); break;
                case Op::INDEX_SET: opop(3); break;
                case Op::INDEX_GET: opop(2); opush(-1); break;
                case Op::MEMBER_GET: opop(1); opush(-1); break;   // [obj] -> [field]
                case Op::MEMBER_SET: opop(2); break;              // [obj, val] -> []
                case Op::ADD: case Op::SUB: case Op::MUL: case Op::ADD_INPLACE:
                case Op::BIT_AND: case Op::BIT_OR: case Op::BIT_XOR: opop(2); opush(-1); break;
                case Op::LESS_JF: case Op::LESS_EQ_JF: case Op::GREATER_JF:
                case Op::GREATER_EQ_JF: case Op::EQUAL_JF: case Op::NOT_EQUAL_JF: opop(2); break;
                case Op::LT_I_JF: case Op::LE_I_JF: case Op::GT_I_JF:
                case Op::GE_I_JF: case Op::EQ_I_JF: case Op::NE_I_JF: opop(1); break;
                case Op::ADD_I: case Op::SUB_I: case Op::MUL_I: case Op::MOD_I:
                case Op::BAND_I: case Op::BOR_I: case Op::BXOR_I: opop(1); opush(-1); break;
                default: break;
            }
            depth += d;
        }
        if (depth < 0) return false;
        if (depth > maxDepth) maxDepth = depth;
        off += len;
    }

    { std::vector<int> keep;
      for (int g : globalOrder) if (!calleeGlobals.count(g)) keep.push_back(g);
      globalOrder.swap(keep); }

    // Build cells with their runtime-observed types.
    auto addCell = [&](bool isGlobal, int idx, int pinned) -> bool {
        const Value& v = isGlobal ? rtGlobals[idx] : rtSlots[idx];
        VKind k = v.tag();
        if (isGlobal && !rtDefined[idx]) return false;
        bool dirty = isGlobal ? (bool)dirtyG.count(idx) : (bool)dirtyL.count(idx);
        bool isBase = isGlobal ? (bool)objBaseG.count(idx) : (bool)objBaseL.count(idx);
        bool isStructBase = isGlobal ? (bool)structBaseG.count(idx) : (bool)structBaseL.count(idx);
        const void* shape = nullptr;
        int vt;
        if (isStructBase) {
            // struct member base: a STRUCT instance, read-only across the region
            // (its POINTER is stable; fields mutate via MEMBER_SET into the slots).
            if (k != VKind::OBJ || !v.isObjType(ObjectType::STRUCT) || dirty) return false;
            vt = VT_STRUCT;
            shape = isGlobal ? gShape[idx] : lShape[idx];
        } else if (isBase) {
            // list index base: a LIST object, read-only across the region (a
            // reassignment mid-region would invalidate the cached pointer).
            if (k != VKind::OBJ || !v.isObjType(ObjectType::LIST) || dirty) return false;
            vt = VT_OBJ;
        } else if (!trTypeOf(k, vt)) {
            return false;
        }
        int ci = (int)cells.size();
        cells.push_back(Cell{isGlobal, idx, vt, (bool)pinned, dirty, -1, shape});
        if (isGlobal) globalCell[idx] = ci; else localCell[idx] = ci;
        return true;
    };
    for (int slot : localOrder)  if (!addCell(false, slot, 1)) return false;
    for (int g : globalOrder)    if (!addCell(true, g, defd.count(g) ? 0 : 1)) return false;

    // ---- register files ----
    // GP: no locals -> RBX joins the pool (globals-only region needs no slots
    // base); R15 always free (no consts base). XMM: all eight are the float file.
    bool haveLocals = !localOrder.empty();
    gpPool = { RSI, RDI, RBP, R8, R9, R10, R11, R15 };
    if (!haveLocals) gpPool.push_back(RBX);
    xmmPool = { XMM0, XMM1, XMM2, XMM3, XMM4, XMM5, XMM6, XMM7 };

    int gi = 0, xi = 0;
    for (auto& c : cells) if (c.pinned) {
        if (c.vt == VT_NUM) { if (xi >= (int)xmmPool.size()) return false; c.poolIdx = xi++; }
        else                { if (gi >= (int)gpPool.size())  return false; c.poolIdx = gi++; }
    }
    numGpPinned = gi; numXmmPinned = xi;
    if (numGpPinned + maxDepth > (int)gpPool.size())  return false;
    if (numXmmPinned + maxDepth > (int)xmmPool.size()) return false;
    if (maxDepth + 1 > (int)otype.size()) otype.assign(maxDepth + 2, VT_INT);

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

    for (auto& c : cells) {
        int mBase = c.isGlobal ? (int)R_GLOBALS : (int)R_SLOTS;
        int disp  = c.idx * 8;
        if (c.isGlobal) {
            a.movRM(RCX, R_CTX, offsetof(JitCtx, globalDefined));
            a.movzxRM8(RAX, RCX, c.idx);
            a.cmpRI(RAX, 0);
            Label okD; a.jcc(NE, okD); a.jmp(prologueBail); a.bind(okD);
        }
        if (c.pinned && c.vt == VT_NUM) {
            a.movRM(RAX, (Reg)mBase, disp); guardFloat(RAX, prologueBail);
            a.movqXR(xmCell((int)(&c - &cells[0])), RAX);
        } else if (c.pinned && c.vt == VT_OBJ) {
            // list index base: load the raw word and guard it is a heap object AND
            // a LIST here, ONCE. The cell is read-only for the region, so every
            // trusted indexAddr afterwards only needs the per-access bounds check.
            Reg r = gpCell((int)(&c - &cells[0]));
            a.movRM(r, (Reg)mBase, disp);
            a.movRR(RCX, r); a.shrRI(RCX, JIT_TAG_SHIFT); a.cmpRI(RCX, (int32_t)TOP17_OBJ);
            Label okO; a.jcc(E, okO); a.jmp(prologueBail); a.bind(okO);
            a.movAbs(RCX, JIT_PAYLOAD); a.movRR(RAX, r); a.andRR(RAX, RCX);      // Object*
            a.movzxRM8(RCX, RAX, OBJ_TAG_OFF); a.cmpRI(RCX, LIST_TAG);
            Label okL; a.jcc(E, okL); a.jmp(prologueBail); a.bind(okL);
        } else if (c.pinned && c.vt == VT_STRUCT) {
            // struct member base: guard object + STRUCT tag + the EXACT shape ONCE
            // (monomorphic). The read-only cell then serves every member access as a
            // fixed-slot load/store — no per-access type/shape check on the base.
            Reg r = gpCell((int)(&c - &cells[0]));
            a.movRM(r, (Reg)mBase, disp);
            a.movRR(RCX, r); a.shrRI(RCX, JIT_TAG_SHIFT); a.cmpRI(RCX, (int32_t)TOP17_OBJ);
            Label okO; a.jcc(E, okO); a.jmp(prologueBail); a.bind(okO);
            a.movAbs(RCX, JIT_PAYLOAD); a.movRR(RAX, r); a.andRR(RAX, RCX);      // StructInstance*
            a.movzxRM8(RCX, RAX, OBJ_TAG_OFF); a.cmpRI(RCX, STRUCT_TAG);
            Label okS; a.jcc(E, okS); a.jmp(prologueBail); a.bind(okS);
            a.movRM(RCX, RAX, STRUCT_SHAPE_OFF); a.movAbs(RDX, (uint64_t)(uintptr_t)c.shape);
            a.cmpRR(RCX, RDX);
            Label okSh; a.jcc(E, okSh); a.jmp(prologueBail); a.bind(okSh);
        } else if (c.pinned) {
            Reg r = gpCell((int)(&c - &cells[0]));
            a.movRM(r, (Reg)mBase, disp); guardUnboxInt(r, prologueBail);
        } else {
            // memory global: guard its current type matches the recorded type
            a.movRM(RAX, R_GLOBALS, disp);
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

        switch (op) {
            case Op::GET_LOCAL: {
                int ci = localCell[(int)rdU16(off + 1)];
                if (cells[ci].vt == VT_NUM) a.movsdRR(xm(depth), xmCell(ci));
                else                        a.movRR(gp(depth), gpCell(ci));
                otype[depth] = cells[ci].vt; depth++; break;
            }
            case Op::SET_LOCAL: {
                int ci = localCell[(int)rdU16(off + 1)];
                if (cells[ci].vt == VT_NUM) { if (otype[depth-1] != VT_NUM) ok = false; else a.movsdRR(xmCell(ci), xm(depth-1)); }
                else                        { ensureInt(depth - 1); if (ok) a.movRR(gpCell(ci), gp(depth-1)); }
                depth--; break;
            }
            case Op::GET_GLOBAL: {
                int g = (int)rdU16(off + 1);
                if (calleeGlobals.count(g)) {
                    a.movRM(gp(depth), R_GLOBALS, g * 8);
                    otype[depth] = VT_CALLEE; depth++; break;
                }
                int ci = globalCell[g];
                if (cells[ci].vt == VT_NUM) {
                    if (cells[ci].pinned) a.movsdRR(xm(depth), xmCell(ci));
                    else                  a.movsdXM(xm(depth), R_GLOBALS, cells[ci].idx * 8);
                } else {
                    if (cells[ci].pinned) a.movRR(gp(depth), gpCell(ci));
                    else { a.movRM(gp(depth), R_GLOBALS, cells[ci].idx * 8);
                           a.shlRI(gp(depth), 17); a.sarRI(gp(depth), 17); }  // memory int stored boxed
                }
                otype[depth] = cells[ci].vt; depth++; break;
            }
            case Op::SET_GLOBAL: case Op::DEFINE_GLOBAL: {
                int ci = globalCell[(int)rdU16(off + 1)];
                if (cells[ci].vt == VT_NUM) {
                    if (otype[depth-1] != VT_NUM) { ok = false; break; }
                    if (cells[ci].pinned) a.movsdRR(xmCell(ci), xm(depth-1));
                    else                  a.movsdMX(R_GLOBALS, cells[ci].idx * 8, xm(depth-1));
                } else {
                    ensureInt(depth - 1); if (!ok) break;
                    if (cells[ci].pinned) a.movRR(gpCell(ci), gp(depth-1));
                    else { boxTo(RDX, gp(depth-1)); a.movMR(R_GLOBALS, cells[ci].idx * 8, RDX); }
                }
                depth--; break;
            }
            case Op::CONST: {
                const Value& k = chunk.consts[rdU16(off + 1)];
                if (k.tag() == VKind::INT) {
                    a.movAbs(gp(depth), (uint64_t)k.asInt()); otype[depth] = VT_INT;
                } else {
                    uint64_t bits; double d = k.asFloat(); std::memcpy(&bits, &d, 8);
                    a.movAbs(RAX, bits); a.movqXR(xm(depth), RAX); otype[depth] = VT_NUM;
                }
                depth++; break;
            }
            case Op::POP: depth--; break;
            case Op::DUP: {
                if (otype[depth-1] == VT_NUM) a.movsdRR(xm(depth), xm(depth-1));
                else                          a.movRR(gp(depth), gp(depth-1));
                otype[depth] = otype[depth-1]; depth++; break;
            }

            case Op::FALSE_: a.movAbs(gp(depth), JIT_TAGS | ((uint64_t)2 << JIT_TAG_SHIFT)); otype[depth]=VT_WORD; depth++; break;
            case Op::TRUE_:  a.movAbs(gp(depth), JIT_TAGS | ((uint64_t)3 << JIT_TAG_SHIFT)); otype[depth]=VT_WORD; depth++; break;
            case Op::NIL:    a.movAbs(gp(depth), JIT_TAGS | ((uint64_t)1 << JIT_TAG_SHIFT)); otype[depth]=VT_WORD; depth++; break;

            case Op::INDEX_SET: {
                // [obj, idx, val] -> list[idx] = val. Base is a pinned LIST cell
                // (VT_OBJ); index int; value a constant word (bool/nil) or a number
                // (no carried pointer, so no write barrier). A pointer value bails.
                if (otype[depth-3] != VT_OBJ) { ok = false; break; }
                ensureInt(depth - 2); if (!ok) break;
                int vvt = otype[depth-1];
                if (vvt == VT_OBJ || vvt == VT_CALLEE) { ok = false; break; }  // no barrier here
                indexAddr(gp(depth-3), gp(depth-2), off, depth, true);   // &elem -> RAX
                if (vvt == VT_INT)      { boxTo(RDX, gp(depth-1)); a.movMR(RAX, 0, RDX); }
                else if (vvt == VT_NUM) { a.movsdMX(RAX, 0, xm(depth-1)); }
                else                    { a.movMR(RAX, 0, gp(depth-1)); }        // VT_WORD raw
                depth -= 3; break;
            }

            case Op::LGET2: {
                for (int w = 0; w < 2; ++w) {
                    int ci = localCell[(int)rdU16(off + 1 + w * 2)];
                    if (cells[ci].vt == VT_NUM) a.movsdRR(xm(depth), xmCell(ci));
                    else                        a.movRR(gp(depth), gpCell(ci));
                    otype[depth] = cells[ci].vt; depth++;
                }
                break;
            }
            case Op::MEMBER_GET: {
                // [obj] -> obj.field. Base is a pinned VT_STRUCT (shape guarded in
                // the prologue), so no re-guard: extract the instance, load the
                // inline-Value slot, and guard the field's recorded numeric type
                // (fields are dynamically typed, so a type change side-exits).
                if (otype[depth-1] != VT_STRUCT) { ok = false; break; }
                int slot = memberSlot[off], fvt = memberFType[off];
                a.movAbs(RCX, JIT_PAYLOAD); a.movRR(RAX, gp(depth-1)); a.andRR(RAX, RCX);  // StructInstance*
                a.movRM(RAX, RAX, STRUCT_SLOTS_OFF);              // slots.begin()
                a.movRM(RCX, RAX, slot * 8);                      // field word
                if (fvt == VT_INT) {
                    a.movRR(RDX, RCX); a.shrRI(RDX, JIT_TAG_SHIFT); a.cmpRI(RDX, (int32_t)JIT_TOP17_INT);
                    Label okI; a.jcc(E, okI); bailTo(off, depth); a.bind(okI);
                    a.shlRI(RCX, 17); a.sarRI(RCX, 17);           // unbox
                    a.movRR(gp(depth-1), RCX); otype[depth-1] = VT_INT;
                } else {                                          // VT_NUM (float field)
                    a.movRR(RDX, RCX); a.movAbs(RAX, JIT_TAGS); a.andRR(RDX, RAX); a.cmpRR(RDX, RAX);
                    Label okF; a.jcc(NE, okF); bailTo(off, depth); a.bind(okF);
                    a.movqXR(xm(depth-1), RCX); otype[depth-1] = VT_NUM;
                }
                break;
            }
            case Op::MEMBER_SET: {
                // [obj, val] -> obj.field = val. A numeric/word value stores inline
                // with no barrier; a pointer value would need a barrier -> decline.
                if (otype[depth-2] != VT_STRUCT) { ok = false; break; }
                int slot = memberSlot[off], vvt = otype[depth-1];
                if (vvt == VT_OBJ || vvt == VT_CALLEE || vvt == VT_STRUCT) { ok = false; break; }
                a.movAbs(RCX, JIT_PAYLOAD); a.movRR(RAX, gp(depth-2)); a.andRR(RAX, RCX);  // StructInstance*
                a.movRM(RAX, RAX, STRUCT_SLOTS_OFF);              // slots.begin()
                if (vvt == VT_NUM)      { a.movsdMX(RAX, slot * 8, xm(depth-1)); }
                else if (vvt == VT_INT) { boxTo(RDX, gp(depth-1)); a.movMR(RAX, slot * 8, RDX); }
                else                    { a.movMR(RAX, slot * 8, gp(depth-1)); }  // VT_WORD raw
                depth -= 2; break;
            }
            case Op::INDEX_GET: {
                if (otype[depth-2] != VT_OBJ) { ok = false; break; }
                ensureInt(depth - 1); if (!ok) break;
                indexAddr(gp(depth-2), gp(depth-1), off, depth, true);   // &elem -> RAX
                a.movRM(gp(depth-2), RAX, 0);                       // load raw element word
                otype[depth-2] = VT_WORD;                           // element type unknown
                depth--; break;
            }
            case Op::JUMP_IF_FALSE: {
                size_t target = next + rdU16(off + 1);
                int vt = otype[depth-1];
                if (vt == VT_INT) {
                    Reg r = gp(depth-1); depth--;
                    a.cmpRI(r, 0);
                    Label fall; a.jcc(NE, fall); branchTo(target); a.bind(fall);
                } else if (vt == VT_WORD || vt == VT_OBJ) {
                    // implement bool/nil truthiness; any other tag bails (val intact)
                    a.movRR(RCX, gp(depth-1)); a.shrRI(RCX, JIT_TAG_SHIFT);
                    Label okType;
                    a.cmpRI(RCX, (int32_t)JIT_TOP17_TRUE);  a.jcc(E, okType);
                    a.cmpRI(RCX, (int32_t)JIT_TOP17_FALSE); a.jcc(E, okType);
                    a.cmpRI(RCX, (int32_t)JIT_TOP17_NIL);   a.jcc(E, okType);
                    bailTo(off, depth);                     // unexpected type -> interpreter
                    a.bind(okType);
                    depth--;
                    a.cmpRI(RCX, (int32_t)JIT_TOP17_TRUE);
                    Label fall; a.jcc(E, fall);             // true -> fall (truthy)
                    branchTo(target);                       // nil/false -> branch
                    a.bind(fall);
                } else {
                    ok = false;                             // float/other -> decline the region
                }
                break;
            }

            case Op::ADD: case Op::ADD_INPLACE: numericBinary(0, depth, off, depth); depth--; break;
            case Op::SUB: numericBinary(1, depth, off, depth); depth--; break;
            case Op::MUL: numericBinary(2, depth, off, depth); depth--; break;

            case Op::BIT_AND: case Op::BIT_OR: case Op::BIT_XOR:
                bitBinary(op, depth); depth--; break;

            case Op::ADD_I: case Op::SUB_I: case Op::MUL_I:
            case Op::BAND_I: case Op::BOR_I: case Op::BXOR_I:
                immBinary(op, (int16_t)rdU16(off + 1), depth, off, depth); break;
            case Op::MOD_I: {
                if (otype[depth-1] != VT_INT) { ok = false; break; }
                int16_t k = (int16_t)rdU16(off + 1);
                if (k == 0) return false;
                modBinary(k, depth); break;
            }

            case Op::LESS_JF: case Op::LESS_EQ_JF:
            case Op::GREATER_JF: case Op::GREATER_EQ_JF:
            case Op::EQUAL_JF: case Op::NOT_EQUAL_JF: {
                uint16_t d = rdU16(off + 1);
                size_t target = next + d;
                if (!numOperand(depth-2) || !numOperand(depth-1)) { ok = false; break; }
                if (otype[depth - 2] == VT_INT && otype[depth - 1] == VT_INT) {
                    Reg lhs = gp(depth - 2), rhs = gp(depth - 1);
                    depth -= 2;
                    a.cmpRR(lhs, rhs);
                    Cond keep = op == Op::LESS_JF ? L : op == Op::LESS_EQ_JF ? LE
                              : op == Op::GREATER_JF ? G : op == Op::GREATER_EQ_JF ? GE
                              : op == Op::EQUAL_JF ? E : NE;
                    Label fall; a.jcc(keep, fall); branchTo(target); a.bind(fall);
                } else {
                    toX(depth - 2); toX(depth - 1);
                    a.ucomisd(xm(depth - 2), xm(depth - 1));
                    Label okN; a.jcc(NP, okN); bailTo(off, depth); a.bind(okN);
                    depth -= 2;
                    Cond keep = op == Op::LESS_JF ? B : op == Op::LESS_EQ_JF ? BE
                              : op == Op::GREATER_JF ? A : op == Op::GREATER_EQ_JF ? AE
                              : op == Op::EQUAL_JF ? E : NE;
                    Label fall; a.jcc(keep, fall); branchTo(target); a.bind(fall);
                }
                break;
            }

            case Op::LT_I_JF: case Op::LE_I_JF: case Op::GT_I_JF:
            case Op::GE_I_JF: case Op::EQ_I_JF: case Op::NE_I_JF: {
                ensureInt(depth - 1); if (!ok) break;
                int16_t k = (int16_t)rdU16(off + 1);
                size_t target = next + rdU16(off + 3);
                Reg r = gp(depth - 1);
                depth -= 1;
                a.movAbs(RCX, (uint64_t)(int64_t)k);
                a.cmpRR(r, RCX);
                Cond keep = op == Op::LT_I_JF ? L : op == Op::LE_I_JF ? LE
                          : op == Op::GT_I_JF ? G : op == Op::GE_I_JF ? GE
                          : op == Op::EQ_I_JF ? E : NE;
                Label fall; a.jcc(keep, fall); branchTo(target); a.bind(fall);
                break;
            }

            case Op::CALL: {
                int argc = calleeArgc[off];
                int calleeSlot = depth - argc - 1;
                // guard the callee global still holds the exact recorded object
                a.movAbs(RCX, calleeWord[off]);
                a.cmpRR(gp(calleeSlot), RCX);
                Label okC; a.jcc(E, okC); bailTo(off, depth); a.bind(okC);
                auto ii = intrinsicAt.find(off);
                if (ii != intrinsicAt.end()) {
                    // sqrt(x): coerce the arg to a double, guard it is >= 0 (the
                    // builtin errors on a negative), then sqrtsd into the result slot.
                    toX(depth - 1);                          // arg -> xm(depth-1)
                    a.xorpd(XMM0, XMM0);                      // 0.0
                    a.ucomisd(xm(depth - 1), XMM0);
                    Label okp; a.jcc(AE, okp); bailTo(off, depth); a.bind(okp);  // arg<0 or NaN -> bail
                    a.sqrtsd(xm(calleeSlot), xm(depth - 1));
                    otype[calleeSlot] = VT_NUM;
                    depth -= argc;
                    break;
                }
                auto it = inlineProto.find(off);
                if (it == inlineProto.end()) return false;
                emitInlineCallee(it->second, depth, argc, off);
                if (!ok) return false;
                depth -= argc;
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
        if (!c.pinned || !c.dirty) continue;
        int mBase = c.isGlobal ? (int)R_GLOBALS : (int)R_SLOTS;
        int idx = (int)(&c - &cells[0]);
        if (c.vt == VT_NUM) {
            a.movsdMX((Reg)mBase, c.idx * 8, xmCell(idx));
        } else {
            boxTo(RDX, gpCell(idx)); a.movMR((Reg)mBase, c.idx * 8, RDX);
        }
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
