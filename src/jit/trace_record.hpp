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

// JIT allocation helper (RFC-027): a trace calls this to build a small list
// literal without leaving native code. gcAlloc never collects mid-allocation —
// it only flags gcPending for the next VM safepoint — so this is GC-safe: the
// trace bails at its LOOP back-edge, where the value stack is the full root set,
// and the collection (if due) runs there. `elems` points at n boxed Value words
// the trace spilled to its native stack; the result is the boxed list word.
inline uint64_t lovaxJitMakeList(const uint64_t* elems, long long n) {
    ListObject* lst = gcAlloc<ListObject>();
    lst->elements.reserve((size_t)n);
    for (long long i = 0; i < n; ++i) {
        Value v; std::memcpy(&v, &elems[i], sizeof(uint64_t));
        lst->elements.push_back(v);
    }
    Value ov = Value::object(Ref<Object>(lst));
    uint64_t w; std::memcpy(&w, &ov, sizeof(uint64_t));
    return w;
}

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
    static constexpr int32_t  VEC_START_OFF = 32;    // offsetof(ListObject,elements) = SmallVec b_
    static constexpr int32_t  VEC_FINISH_OFF= 40;    // SmallVec e_ (end pointer)
    static constexpr int32_t  VEC_CAP_OFF   = 48;    // SmallVec c_ (capacity pointer)
    static constexpr int      LIST_TAG      = 5;     // ObjectType::LIST
    // struct instance (Stage-8 G2): shape ptr + inline-Value slots (SmallVec begin).
    static constexpr int32_t  STRUCT_SHAPE_OFF = 32; // offsetof(StructInstanceObject, shape)
    static constexpr int32_t  STRUCT_SLOTS_OFF = 40; // offsetof(slots) + SmallVec begin(0)
    static constexpr int      STRUCT_TAG       = 17; // ObjectType::STRUCT
    // for-range iteration (FOR_NEXT). Offsets verified via offsetof for the 8-byte
    // Value build (the only build the JIT runs in).
    static constexpr int32_t  ITER_KIND_OFF    = 28; // IterObject::kind (RANGE==1)
    static constexpr int32_t  ITER_SOURCE_OFF  = 32; // IterObject::source (raw RangeObject*)
    static constexpr int32_t  ITER_INDEX_OFF   = 64; // IterObject::index
    static constexpr int32_t  RANGE_END_OFF    = 40; // RangeObject::end
    static constexpr int32_t  RANGE_STEP_OFF   = 48; // RangeObject::step
    static constexpr int       ITER_KIND_RANGE  = 1;  // IterObject::Kind::RANGE

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
    std::unordered_map<size_t, const void*> memberShape;      // MEMBER off -> shape guarded per access
    std::unordered_map<size_t, int> indexEType;               // INDEX_GET off -> speculated element VT (INT/NUM)

    std::vector<Reg> gpPool;      // INT/CALLEE cells (pinned) then INT/CALLEE operands
    std::vector<Xmm> xmmPool;     // NUM cells (pinned) then NUM operands
    int numGpPinned = 0, numXmmPinned = 0;
    int maxDepth = 0;
    std::vector<int> otype;       // operand type by depth

    struct Snapshot { size_t resume; int depth; };
    std::vector<Snapshot> snaps;

    std::unordered_set<int> calleeGlobals;

    // ---- ABC: array bounds check elision for a canonical counting loop ----
    // A `while C < N` loop whose counter C indexes read-only lists lets each
    // per-access bounds check drop, guarded ONCE at entry (C0>=0 and size>maxIdx).
    // Conservative: detected only for the exact shape below; any deviation leaves
    // every check in. `abcCounterOrig` uses the cell-origin encoding (global>=0,
    // local as -(2+slot)), valid only when abcActive.
    bool   abcActive = false;
    int    abcCounterOrig = 0;
    long long abcMaxIdx = -1;                    // largest value C reaches at a safe access
    size_t abcIncOff = 0;                        // bytecode offset of the C = C + K write
    std::unordered_map<size_t,bool> abcSafeAccess;   // INDEX off -> skip its bounds check
    std::unordered_set<int> abcBase;                 // base cell origins needing the entry size guard
    // LICM: a read-only pinned base ALL of whose accesses are ABC-safe caches its
    // elements-begin pointer in its register at entry (instead of the raw word), so
    // a per-access address is just begin + idx*8 (no mask, no start load, no bounds).
    std::unordered_map<int,int> baseAccTotal, baseAccSafe;   // per base origin
    std::unordered_map<size_t,int> accBaseOrig;              // INDEX off -> base origin
    std::unordered_set<int> beginBase;                       // origins whose register holds begin
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
    // guard the raw word in `objR` is an object, a STRUCT, and exactly `shape`;
    // leaves StructInstance* in RAX. Bails otherwise. Clobbers RAX/RCX/RDX.
    void guardStructPtr(Reg objR, const void* shape, size_t off, int depth) {
        a.movRR(RCX, objR); a.shrRI(RCX, JIT_TAG_SHIFT); a.cmpRI(RCX, (int32_t)TOP17_OBJ);
        Label o1; a.jcc(E, o1); bailTo(off, depth); a.bind(o1);
        a.movAbs(RCX, JIT_PAYLOAD); a.movRR(RAX, objR); a.andRR(RAX, RCX);      // StructInstance*
        a.movzxRM8(RCX, RAX, OBJ_TAG_OFF); a.cmpRI(RCX, STRUCT_TAG);
        Label o2; a.jcc(E, o2); bailTo(off, depth); a.bind(o2);
        a.movRM(RCX, RAX, STRUCT_SHAPE_OFF); a.movAbs(RDX, (uint64_t)(uintptr_t)shape); a.cmpRR(RCX, RDX);
        Label o3; a.jcc(E, o3); bailTo(off, depth); a.bind(o3);
    }
    // &list[idx] -> RAX from a raw list word `objR` and unboxed int `idxR`; bails
    // out-of-range (incl. negative). `trusted` skips the object + LIST-tag guard —
    // valid because every VT_OBJ operand originates from a pinned list cell already
    // guarded as an object AND a LIST in the prologue, and such a cell is read-only
    // for the whole region, so only the per-access BOUNDS check is needed (this is
    // the guard-hoisting LICM does for list kernels). Clobbers RAX/RCX/RDX.
    // boundsSafe: ABC has proven 0 <= idx < size for this access (guarded once at
    // entry), so skip the per-access bounds compare — just compute &list[idx].
    void indexAddr(Reg objR, Reg idxR, size_t off, int depth, bool trusted, bool boundsSafe = false,
                   bool baseIsBegin = false) {
        if (baseIsBegin) {                          // objR already holds elements-begin (LICM)
            a.movRR(RAX, idxR); a.shlRI(RAX, 3); a.addRR(RAX, objR);   // &elem = begin + idx*8
            return;
        }
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
        if (!boundsSafe) {
            a.movRM(RCX, RAX, VEC_FINISH_OFF); a.subRR(RCX, RDX);            // RCX = size*8
            a.movRR(RAX, idxR); a.shlRI(RAX, 3);
            a.cmpRR(RAX, RCX);
            Label o3; a.jcc(B, o3); bailTo(off, depth); a.bind(o3);          // unsigned: catches <0
        } else {
            a.movRR(RAX, idxR); a.shlRI(RAX, 3);                             // idx*8; bounds proven at entry
        }
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
        case Op::LGET_ADD_I: case Op::LGET_SUB_I:
        case Op::MEMBER_GET: case Op::MEMBER_SET:
        case Op::INDEX_GET_KEEP: case Op::MEMBER_GET_KEEP:
        case Op::NEGATE: case Op::DIV: case Op::MOD:
        case Op::FOR_NEXT: case Op::CLOSE_UPVALUE:
        case Op::LIST:
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
        case Op::LGET_ADD_I: case Op::LGET_SUB_I: return +1;   // push local +/- k
        case Op::MEMBER_GET: return 0;    // [obj] -> [field]
        case Op::MEMBER_SET: return -2;   // [obj, val] -> []
        case Op::INDEX_GET_KEEP: return +1;   // [base, idx] -> [base, idx, elem]
        case Op::MEMBER_GET_KEEP: return +1;  // [obj] -> [obj, field]
        case Op::NEGATE: return 0;            // [x] -> [-x]
        case Op::ADD: case Op::SUB: case Op::MUL: case Op::ADD_INPLACE:
        case Op::DIV: case Op::MOD:
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

    // ---- ABC pre-analysis (pure bytecode; before the typed walk so the walk can
    // mark safe accesses). Find the single loop-exit guard `LT_I_JF/LE_I_JF k` on a
    // counter loaded immediately before it, then confirm the counter is written
    // exactly once as `C = C + K` (K>0). Any deviation -> abcActive stays false. ----
    {
        auto getOrig = [&](Op pop, size_t poff) -> int {   // cell origin of a GET, or 1 (invalid)
            if (pop == Op::GET_GLOBAL) return (int)rdU16(poff + 1);
            if (pop == Op::GET_LOCAL)  return -2 - (int)rdU16(poff + 1);
            return 1;                                       // 1 is never a valid origin
        };
        size_t guardOff = end, prevOff = end; Op prevOp = Op::HALT;
        long long boundN = 0; bool strict = true; int cOrig = 1; bool multiGuard = false;
        for (size_t o = start; o < end; ) {
            Op op = (Op)chunk.code[o]; int l = instrLength(chunk, o);
            if (op == Op::LT_I_JF || op == Op::LE_I_JF) {
                size_t tgt = o + l + rdU16(o + 3);
                if (tgt >= end) {                           // branch exits the region = loop guard
                    if (guardOff != end) multiGuard = true;
                    guardOff = o; boundN = (int16_t)rdU16(o + 1); strict = (op == Op::LT_I_JF);
                    cOrig = (prevOff != end) ? getOrig(prevOp, prevOff) : 1;
                }
            }
            prevOff = o; prevOp = op; o += l;
        }
        bool counterIsInt = false;
        if (cOrig >= 0)         counterIsInt = rtDefined[cOrig] && rtGlobals[cOrig].tag() == VKind::INT;
        else if (cOrig <= -2)   counterIsInt = rtSlots[-2 - cOrig].tag() == VKind::INT;
        if (!multiGuard && guardOff != end && cOrig != 1 && counterIsInt) {
            // confirm exactly one write to C, of the shape `GET C; CONST K; ADD[_INPLACE]; SET C`
            // or `GET C; ADD_I K; SET C`, with K a positive int.
            int writes = 0; bool goodInc = false; size_t incOff = end;
            size_t w0 = end, w1 = end, w2 = end; Op p0 = Op::HALT, p1 = Op::HALT, p2 = Op::HALT;
            for (size_t o = start; o < end; ) {
                Op op = (Op)chunk.code[o]; int l = instrLength(chunk, o);
                bool isSet = (op == Op::SET_GLOBAL && cOrig >= 0 && (int)rdU16(o + 1) == cOrig)
                          || (op == Op::SET_LOCAL  && cOrig <= -2 && (-2 - (int)rdU16(o + 1)) == cOrig);
                if (isSet) {
                    writes++; incOff = o;
                    if ((p0 == Op::ADD || p0 == Op::ADD_INPLACE) && p1 == Op::CONST && getOrig(p2, w2) == cOrig) {
                        const Value& kv = chunk.consts[rdU16(w1 + 1)];
                        if (kv.tag() == VKind::INT && kv.asInt() > 0) goodInc = true;
                    } else if (p0 == Op::ADD_I && getOrig(p1, w1) == cOrig) {
                        if ((int16_t)rdU16(w0 + 1) > 0) goodInc = true;
                    }
                }
                w2 = w1; p2 = p1; w1 = w0; p1 = p0; w0 = o; p0 = op; o += l;
            }
            long long maxIdx = strict ? boundN - 1 : boundN;
            if (writes == 1 && goodInc && maxIdx >= 0) {
                abcActive = true; abcCounterOrig = cOrig; abcMaxIdx = maxIdx; abcIncOff = incOff;
            }
        }
    }

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
        if (op == Op::GET_LOCAL || op == Op::SET_LOCAL ||
            op == Op::LGET_ADD_I || op == Op::LGET_SUB_I) {
            int slot = (int)rdU16(off + 1);
            if (!localCell.count(slot)) { localCell[slot] = -1; localOrder.push_back(slot); }
            if (op == Op::SET_LOCAL) dirtyL[slot] = true;
        }
        if (op == Op::FOR_NEXT) {                        // range for-loop: writes the loop var (v1)
            if (chunk.code[off + 1] & 2) return false;   // pair (for k,v) not traced
            int slot = (int)rdU16(off + 2);
            if (!localCell.count(slot)) { localCell[slot] = -1; localOrder.push_back(slot); }
            dirtyL[slot] = true;                         // FOR_NEXT assigns it each iteration
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
            // Math intrinsic: sqrt/floor/ceil(x) — a native builtin call in a hot
            // float loop is emitted as an SSE instruction instead of declining the
            // region. Guarded that the global still holds this exact builtin at run
            // time. floor/ceil match the interpreter's (long long)std::floor/ceil
            // bit-for-bit (roundsd then cvttsd2si = the C++ cast on x86-64); round()
            // is NOT here — its half-away ties differ from roundsd's ties-to-even.
            int mathKind = 0;
            if (ob && ob->tag == ObjectType::BUILTIN && argc == 1) {
                const std::string& bn = static_cast<BuiltinObject*>(ob)->name;
                if (bn == "sqrt") mathKind = 1;
                else if (bn == "floor") mathKind = 2;
                else if (bn == "ceil") mathKind = 3;
                else if (bn == "abs") mathKind = 4;   // type-dependent: emit decides int vs float
            }
            if (mathKind) {
                uint64_t w; std::memcpy(&w, &rtGlobals[g], 8);
                intrinsicAt[off] = mathKind; calleeWord[off] = w; calleeArgc[off] = argc;
                calleeGlobals.insert(g);
                int peak = depth + 1; if (peak > maxDepth) maxDepth = peak;
                opop(argc + 1); opush(-1); depth -= argc;
                off += len; continue;                     // handled; next op
            }
            // push(list, val): a 2-arg builtin. A number value appends in native code
            // (fast path; a full backing side-exits so the interpreter grows it), so
            // building a list in a loop traces. The list variable is read-only (its
            // pointer is stable — only its contents grow), so it is an objBase cell.
            if (ob && ob->tag == ObjectType::BUILTIN && argc == 2 &&
                static_cast<BuiltinObject*>(ob)->name == "push") {
                int listPos = calleePos + 1;              // [callee, list, val]
                int lo = (listPos < (int)orig.size()) ? orig[listPos] : -1;
                if (lo >= 0) objBaseG[lo] = true;
                else if (lo <= -2) objBaseL[-2 - lo] = true;
                else return false;                        // list not a plain cell
                uint64_t w; std::memcpy(&w, &rtGlobals[g], 8);
                intrinsicAt[off] = 5; calleeWord[off] = w; calleeArgc[off] = argc;
                calleeGlobals.insert(g);
                int peak = depth + 1; if (peak > maxDepth) maxDepth = peak;
                opop(argc + 1); opush(-1); depth -= argc;
                off += len; continue;
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
            if (op == Op::INDEX_SET || op == Op::INDEX_GET || op == Op::INDEX_GET_KEEP) {
                // base is below the index (INDEX_GET[_KEEP]: [base idx]; INDEX_SET: [base idx val])
                int b = (int)orig.size() - (op == Op::INDEX_SET ? 3 : 2);
                if (b < 0) return false;
                int o = orig[b];
                if (o >= 0) objBaseG[o] = true;
                else if (o <= -2) objBaseL[-2 - o] = true;
                else return false;                            // base not a plain cell
                // ABC: arr[C] where C is the loop counter and the access precedes
                // the counter's increment -> 0 <= C < N <= size holds (entry guards),
                // so this per-access bounds check can be elided. Record it and the
                // base cell that then needs the one-time entry size guard.
                if (abcActive) {
                    baseAccTotal[o]++;
                    accBaseOrig[off] = o;
                    if (off < abcIncOff && orig[b + 1] == abcCounterOrig) {
                        abcSafeAccess[off] = true;
                        abcBase.insert(o);
                        baseAccSafe[o]++;
                    }
                }
                // INDEX_GET: speculate the element type from the LIVE list (like a
                // struct field's recorded type) so a numeric element refines from a
                // raw word to VT_INT/VT_NUM and downstream arithmetic can trace. A
                // per-access type guard side-exits if the speculation is ever wrong.
                if (op == Op::INDEX_GET || op == Op::INDEX_GET_KEEP) {
                    const Value* bv = (o >= 0) ? &rtGlobals[o] : &rtSlots[-2 - o];
                    if (bv && bv->isObjType(ObjectType::LIST)) {
                        auto* lst = static_cast<ListObject*>(bv->asObj());
                        if (lst->elements.size() > 0) {
                            VKind et = lst->elements[0].tag();
                            if (et == VKind::INT)        indexEType[off] = VT_INT;
                            else if (et == VKind::FLOAT) indexEType[off] = VT_NUM;
                        }
                    }
                }
            }
            if (op == Op::MEMBER_GET || op == Op::MEMBER_SET || op == Op::MEMBER_GET_KEEP) {
                // resolve the member from the LIVE struct instance: field slot +
                // field type + shape (for the prologue monomorphic guard). Numeric
                // fields only; anything else declines the region.
                int b = (op == Op::MEMBER_SET) ? (int)orig.size() - 2 : (int)orig.size() - 1;
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
                memberSlot[off] = slot; memberFType[off] = fvt; memberShape[off] = si->shape;
                if (o >= 0) { structBaseG.insert(o); gShape[o] = si->shape; }
                else { int s = -2 - o; structBaseL.insert(s); lShape[s] = si->shape; }
            }
            int d = traceStackDelta(op);
            if (op == Op::LIST) {                     // small list literal -> traceable alloc
                int n = (int)rdU16(off + 1);
                if (n < 1 || n > 8) return false;     // cap the native-stack element buffer
                opop(n); opush(-1); d = 1 - n;
            }
            switch (op) {
                case Op::GET_GLOBAL: opush((int)rdU16(off + 1)); break;
                case Op::GET_LOCAL: opush(-2 - (int)rdU16(off + 1)); break;
                case Op::LGET2: opush(-2 - (int)rdU16(off + 1)); opush(-2 - (int)rdU16(off + 3)); break;
                case Op::LGET_ADD_I: case Op::LGET_SUB_I: opush(-1); break;   // computed value
                case Op::CONST: case Op::FALSE_: case Op::TRUE_: case Op::NIL: opush(-1); break;
                case Op::DUP: opush(orig.empty() ? -1 : orig.back()); break;
                case Op::SET_LOCAL: case Op::SET_GLOBAL: case Op::DEFINE_GLOBAL:
                case Op::POP: case Op::JUMP_IF_FALSE: opop(1); break;
                case Op::INDEX_SET: opop(3); break;
                case Op::INDEX_GET: opop(2); opush(-1); break;
                case Op::MEMBER_GET: opop(1); opush(-1); break;   // [obj] -> [field]
                case Op::INDEX_GET_KEEP: opush(-1); break;        // keep base+idx, push elem
                case Op::MEMBER_GET_KEEP: opush(-1); break;       // keep obj, push field
                case Op::NEGATE: opop(1); opush(-1); break;       // replace top
                case Op::MEMBER_SET: opop(2); break;              // [obj, val] -> []
                case Op::ADD: case Op::SUB: case Op::MUL: case Op::ADD_INPLACE:
                case Op::DIV: case Op::MOD:
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
            // struct member base: a STRUCT instance. It MAY be reassigned each
            // iteration (`e = ents[j]`, array-of-structs), so the pointer is not
            // stable — every MEMBER access guards the tag+shape per access. The cell
            // just carries the raw pointer word (pinned or in memory).
            if (k != VKind::OBJ || !v.isObjType(ObjectType::STRUCT)) return false;
            vt = VT_STRUCT;
            shape = isGlobal ? gShape[idx] : lShape[idx];
        } else if (isBase) {
            // list index base: a LIST object. Read-only -> the pointer is stable and
            // a pinned/prologue-guarded fast path applies. Reassigned (dirty, e.g.
            // `tmp = [..]` each iteration) -> it must be a memory cell reloaded and
            // obj/LIST-guarded per use, never a stale cached pointer.
            if (k != VKind::OBJ || !v.isObjType(ObjectType::LIST)) return false;
            vt = VT_OBJ;
            if (dirty) pinned = 0;                         // reassigned base -> memory cell
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

    // Register pressure relief for struct-of-arrays loops (xs[j], ys[j], vxs[j],
    // vys[j] = four distinct LIST bases). If the pinned GP cells plus the operand
    // peak overflow the pool, demote read-only GLOBAL list bases to memory cells:
    // each is reloaded (and obj+LIST guarded) at its GET_GLOBAL instead of held in
    // a dedicated register. A base pointer is loop-invariant so the reload is an L1
    // hit — far cheaper than declining the whole region to the template compiler,
    // which runs at interpreter speed. Counter/accumulator cells stay pinned.
    auto gpPinnedCount = [&]{ int n = 0; for (auto& c : cells) if (c.pinned && c.vt != VT_NUM) ++n; return n; };
    for (auto& c : cells) {
        if (gpPinnedCount() + maxDepth <= (int)gpPool.size()) break;
        if (c.pinned && c.vt == VT_OBJ && c.isGlobal) c.pinned = false;
    }

    int gi = 0, xi = 0;
    for (auto& c : cells) if (c.pinned) {
        if (c.vt == VT_NUM) { if (xi >= (int)xmmPool.size()) return false; c.poolIdx = xi++; }
        else                { if (gi >= (int)gpPool.size())  return false; c.poolIdx = gi++; }
    }
    numGpPinned = gi; numXmmPinned = xi;
    if (numGpPinned + maxDepth > (int)gpPool.size())  return false;
    if (numXmmPinned + maxDepth > (int)xmmPool.size()) return false;
    if (maxDepth + 1 > (int)otype.size()) otype.assign(maxDepth + 2, VT_INT);

    // LICM begin-caching eligibility (pinned-ness is now final after demotion): a
    // base qualifies if it is pinned and EVERY one of its accesses is ABC-safe, so
    // its register can hold the elements-begin pointer with no raw word ever needed.
    if (abcActive) for (auto& kv : baseAccTotal) {
        int o = kv.first;
        if (kv.second > 0 && baseAccSafe[o] == kv.second) {
            int ci = (o >= 0) ? globalCell[o] : localCell[-2 - o];
            if (ci >= 0 && cells[ci].pinned && cells[ci].vt == VT_OBJ) beginBase.insert(o);
        }
    }

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
            // struct member base: load the raw pointer word only — it may be
            // reassigned each iteration, so every MEMBER access guards tag+shape.
            (void)c.shape;
            Reg r = gpCell((int)(&c - &cells[0]));
            a.movRM(r, (Reg)mBase, disp);
        } else if (c.pinned) {
            Reg r = gpCell((int)(&c - &cells[0]));
            a.movRM(r, (Reg)mBase, disp); guardUnboxInt(r, prologueBail);
        } else if (c.vt == VT_STRUCT) {
            // memory struct base (e = ents[j] each iteration): no entry guard —
            // every MEMBER access guards tag+shape on the current value itself.
        } else if (c.vt == VT_OBJ && !c.dirty) {
            // memory list base, read-only (demoted under register pressure): guard
            // obj+LIST ONCE here. The base stays a list every iteration, so each
            // GET_GLOBAL just reloads the pointer with no re-guard.
            a.movRM(RAX, (Reg)mBase, disp);
            a.movRR(RCX, RAX); a.shrRI(RCX, JIT_TAG_SHIFT); a.cmpRI(RCX, (int32_t)TOP17_OBJ);
            Label okO; a.jcc(E, okO); a.jmp(prologueBail); a.bind(okO);
            a.movAbs(RCX, JIT_PAYLOAD); a.andRR(RAX, RCX);
            a.movzxRM8(RCX, RAX, OBJ_TAG_OFF); a.cmpRI(RCX, LIST_TAG);
            Label okL; a.jcc(E, okL); a.jmp(prologueBail); a.bind(okL);
        } else if (c.vt == VT_OBJ) {
            // memory list base, reassigned each iteration (dirty): its entry value is
            // overwritten before use, so no entry guard — each GET_GLOBAL reloads and
            // obj/LIST-guards the current value per use.
        } else {
            // memory global: guard its current type matches the recorded type
            a.movRM(RAX, R_GLOBALS, disp);
            if (c.vt == VT_INT) guardUnboxInt(RAX, prologueBail);
            else                guardFloat(RAX, prologueBail);
        }
    }

    // ---- ABC entry guards: make the elided per-access bounds checks sound ----
    // Run once per native entry. The counter only increases from its entry value,
    // and no traced op can push/shrink a read-only list, so guarding C0 >= 0 and
    // size(base) > maxIdx here proves 0 <= C <= maxIdx < size for every iteration.
    if (abcActive) {
        int cci = (abcCounterOrig >= 0) ? globalCell[abcCounterOrig]
                                        : localCell[-2 - abcCounterOrig];
        a.cmpRI(gpCell(cci), 0);                              // counter is a pinned unboxed VT_INT
        Label okC; a.jcc(GE, okC); a.jmp(prologueBail); a.bind(okC);
        int needBytes = (int)((abcMaxIdx + 1) * 8);          // size*8 must be at least this
        for (int bo : abcBase) {
            int bci = (bo >= 0) ? globalCell[bo] : localCell[-2 - bo];
            if (cells[bci].pinned) a.movRR(RAX, gpCell(bci));
            else                   a.movRM(RAX, (Reg)(bo >= 0 ? R_GLOBALS : R_SLOTS), cells[bci].idx * 8);
            a.movAbs(RCX, JIT_PAYLOAD); a.andRR(RAX, RCX);   // Object*
            a.movRM(RDX, RAX, VEC_START_OFF);
            a.movRM(RCX, RAX, VEC_FINISH_OFF); a.subRR(RCX, RDX);   // RCX = size*8
            a.cmpRI(RCX, needBytes);
            Label okS; a.jcc(AE, okS); a.jmp(prologueBail); a.bind(okS);
        }
        // LICM: overwrite each begin-cacheable base register with its elements-begin
        // pointer (done after the size guards, which still needed the raw word). The
        // base is read-only and the GC is non-moving, so begin stays valid.
        for (int o : beginBase) {
            int ci = (o >= 0) ? globalCell[o] : localCell[-2 - o];
            Reg r = gpCell(ci);
            a.movAbs(RCX, JIT_PAYLOAD); a.movRR(RAX, r); a.andRR(RAX, RCX);   // Object*
            a.movRM(r, RAX, VEC_START_OFF);                                   // r = begin
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
                else if (cells[ci].vt == VT_STRUCT) {   // reassign the entity pointer (raw word)
                    if (otype[depth-1] != VT_STRUCT && otype[depth-1] != VT_WORD) ok = false;
                    else if (cells[ci].pinned) a.movRR(gpCell(ci), gp(depth-1));
                    else a.movMR(R_SLOTS, cells[ci].idx * 8, gp(depth-1));
                }
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
                } else if (cells[ci].vt == VT_STRUCT) {
                    if (cells[ci].pinned) a.movRR(gp(depth), gpCell(ci));
                    else                  a.movRM(gp(depth), R_GLOBALS, cells[ci].idx * 8);  // raw pointer word
                } else if (cells[ci].vt == VT_OBJ) {
                    // pinned: read the register. read-only memory: reload the raw word
                    // (obj+LIST guarded once in the prologue). dirty memory (reassigned
                    // each iteration): reload and obj/LIST-guard the current value here.
                    if (cells[ci].pinned) a.movRR(gp(depth), gpCell(ci));
                    else {
                        Reg r = gp(depth);
                        a.movRM(r, R_GLOBALS, cells[ci].idx * 8);
                        if (cells[ci].dirty) {
                            a.movRR(RCX, r); a.shrRI(RCX, JIT_TAG_SHIFT); a.cmpRI(RCX, (int32_t)TOP17_OBJ);
                            Label okO; a.jcc(E, okO); bailTo(off, depth); a.bind(okO);
                            a.movAbs(RCX, JIT_PAYLOAD); a.movRR(RAX, r); a.andRR(RAX, RCX);
                            a.movzxRM8(RCX, RAX, OBJ_TAG_OFF); a.cmpRI(RCX, LIST_TAG);
                            Label okL; a.jcc(E, okL); bailTo(off, depth); a.bind(okL);
                        }
                    }
                } else {
                    if (cells[ci].pinned) a.movRR(gp(depth), gpCell(ci));
                    else { a.movRM(gp(depth), R_GLOBALS, cells[ci].idx * 8);
                           a.shlRI(gp(depth), 17); a.sarRI(gp(depth), 17); }  // memory int stored boxed
                }
                otype[depth] = cells[ci].vt; depth++; break;
            }
            case Op::SET_GLOBAL: case Op::DEFINE_GLOBAL: {
                int ci = globalCell[(int)rdU16(off + 1)];
                if (cells[ci].vt == VT_STRUCT) {         // reassign the entity pointer (raw word)
                    if (otype[depth-1] != VT_STRUCT && otype[depth-1] != VT_WORD) { ok = false; break; }
                    if (cells[ci].pinned) a.movRR(gpCell(ci), gp(depth-1));
                    else                  a.movMR(R_GLOBALS, cells[ci].idx * 8, gp(depth-1));  // global root: no barrier
                    depth--; break;
                }
                if (cells[ci].vt == VT_OBJ) {            // reassign a list base (raw pointer word)
                    if (otype[depth-1] != VT_OBJ && otype[depth-1] != VT_WORD) { ok = false; break; }
                    if (cells[ci].pinned) a.movRR(gpCell(ci), gp(depth-1));
                    else                  a.movMR(R_GLOBALS, cells[ci].idx * 8, gp(depth-1));  // global root: no barrier
                    depth--; break;
                }
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
                { bool safe = abcSafeAccess.count(off) != 0;
                  bool begin = safe && beginBase.count(accBaseOrig[off]) != 0;
                  indexAddr(gp(depth-3), gp(depth-2), off, depth, true, safe, begin); }   // &elem -> RAX
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
            case Op::LGET_ADD_I: case Op::LGET_SUB_I: {
                // fused GET_LOCAL + ADD_I/SUB_I. Int locals only (the common loop-
                // counter case, e.g. a `[i, i+1, i+2]` literal); a float local
                // declines. Push the local, then reuse the immediate-arith path
                // (range-checked, side-exits on overflow of the inline int range).
                int ci = localCell[(int)rdU16(off + 1)];
                if (cells[ci].vt != VT_INT) { ok = false; break; }
                a.movRR(gp(depth), gpCell(ci)); otype[depth] = VT_INT; depth++;
                immBinary(op == Op::LGET_ADD_I ? Op::ADD_I : Op::SUB_I,
                          (int16_t)rdU16(off + 3), depth, off, depth);
                break;
            }
            case Op::MEMBER_GET: {
                // [obj] -> obj.field. Base is a pinned VT_STRUCT (shape guarded in
                // the prologue), so no re-guard: extract the instance, load the
                // inline-Value slot, and guard the field's recorded numeric type
                // (fields are dynamically typed, so a type change side-exits).
                { int vtb = otype[depth-1]; if (vtb != VT_STRUCT && vtb != VT_WORD) { ok = false; break; } }
                int slot = memberSlot[off], fvt = memberFType[off];
                guardStructPtr(gp(depth-1), memberShape[off], off, depth);  // StructInstance* -> RAX
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
                { int vtb = otype[depth-2]; if (vtb != VT_STRUCT && vtb != VT_WORD) { ok = false; break; } }
                int slot = memberSlot[off], vvt = otype[depth-1];
                if (vvt == VT_OBJ || vvt == VT_CALLEE || vvt == VT_STRUCT) { ok = false; break; }
                guardStructPtr(gp(depth-2), memberShape[off], off, depth);  // StructInstance* -> RAX
                a.movRM(RAX, RAX, STRUCT_SLOTS_OFF);              // slots.begin()
                if (vvt == VT_NUM)      { a.movsdMX(RAX, slot * 8, xm(depth-1)); }
                else if (vvt == VT_INT) { boxTo(RDX, gp(depth-1)); a.movMR(RAX, slot * 8, RDX); }
                else                    { a.movMR(RAX, slot * 8, gp(depth-1)); }  // VT_WORD raw
                depth -= 2; break;
            }
            case Op::INDEX_GET: {
                if (otype[depth-2] != VT_OBJ) { ok = false; break; }
                ensureInt(depth - 1); if (!ok) break;
                { bool safe = abcSafeAccess.count(off) != 0;
                  bool begin = safe && beginBase.count(accBaseOrig[off]) != 0;
                  indexAddr(gp(depth-2), gp(depth-1), off, depth, true, safe, begin); }   // &elem -> RAX (base reg preserved)
                auto ie = indexEType.find(off);
                if (ie != indexEType.end() && ie->second == VT_NUM) {
                    // speculated float element: load to scratch, guard it IS a float,
                    // then move into the XMM slot. Guard bails with the base still
                    // live in gp(depth-2), so the side-exit snapshot stays correct.
                    a.movRM(RCX, RAX, 0);                          // element word -> RCX
                    a.movRR(RDX, RCX); a.movAbs(RAX, JIT_TAGS); a.andRR(RDX, RAX); a.cmpRR(RDX, RAX);
                    Label okF; a.jcc(NE, okF); bailTo(off, depth); a.bind(okF);   // (w&TAGS)==TAGS -> not float
                    a.movqXR(xm(depth-2), RCX); otype[depth-2] = VT_NUM;
                } else if (ie != indexEType.end() && ie->second == VT_INT) {
                    // speculated int element: guard the INT tag, then unbox in place.
                    a.movRM(RCX, RAX, 0);                          // element word -> RCX
                    a.movRR(RDX, RCX); a.shrRI(RDX, JIT_TAG_SHIFT); a.cmpRI(RDX, (int32_t)JIT_TOP17_INT);
                    Label okI; a.jcc(E, okI); bailTo(off, depth); a.bind(okI);
                    a.shlRI(RCX, 17); a.sarRI(RCX, 17);            // unbox
                    a.movRR(gp(depth-2), RCX); otype[depth-2] = VT_INT;
                } else {
                    a.movRM(gp(depth-2), RAX, 0);                  // raw word, element type unknown
                    otype[depth-2] = VT_WORD;
                }
                depth--; break;
            }
            case Op::INDEX_GET_KEEP: {
                // like INDEX_GET but the base+index stay on the stack (compound assign
                // a[i] += x); the element lands in a NEW slot on top.
                if (otype[depth-2] != VT_OBJ) { ok = false; break; }
                ensureInt(depth - 1); if (!ok) break;
                { bool safe = abcSafeAccess.count(off) != 0;
                  bool begin = safe && beginBase.count(accBaseOrig[off]) != 0;
                  indexAddr(gp(depth-2), gp(depth-1), off, depth, true, safe, begin); }
                int dst = depth;
                auto ie = indexEType.find(off);
                if (ie != indexEType.end() && ie->second == VT_NUM) {
                    a.movRM(RCX, RAX, 0);
                    a.movRR(RDX, RCX); a.movAbs(RAX, JIT_TAGS); a.andRR(RDX, RAX); a.cmpRR(RDX, RAX);
                    Label okF; a.jcc(NE, okF); bailTo(off, depth); a.bind(okF);
                    a.movqXR(xm(dst), RCX); otype[dst] = VT_NUM;
                } else if (ie != indexEType.end() && ie->second == VT_INT) {
                    a.movRM(RCX, RAX, 0);
                    a.movRR(RDX, RCX); a.shrRI(RDX, JIT_TAG_SHIFT); a.cmpRI(RDX, (int32_t)JIT_TOP17_INT);
                    Label okI; a.jcc(E, okI); bailTo(off, depth); a.bind(okI);
                    a.shlRI(RCX, 17); a.sarRI(RCX, 17);
                    a.movRR(gp(dst), RCX); otype[dst] = VT_INT;
                } else {
                    a.movRM(gp(dst), RAX, 0); otype[dst] = VT_WORD;
                }
                depth++; break;
            }
            case Op::MEMBER_GET_KEEP: {
                // like MEMBER_GET but the object stays on the stack (obj.f += x); the
                // field lands in a NEW slot on top.
                { int vtb = otype[depth-1]; if (vtb != VT_STRUCT && vtb != VT_WORD) { ok = false; break; } }
                int slot = memberSlot[off], fvt = memberFType[off];
                guardStructPtr(gp(depth-1), memberShape[off], off, depth);
                a.movRM(RAX, RAX, STRUCT_SLOTS_OFF);
                a.movRM(RCX, RAX, slot * 8);
                int dst = depth;
                if (fvt == VT_INT) {
                    a.movRR(RDX, RCX); a.shrRI(RDX, JIT_TAG_SHIFT); a.cmpRI(RDX, (int32_t)JIT_TOP17_INT);
                    Label okI; a.jcc(E, okI); bailTo(off, depth); a.bind(okI);
                    a.shlRI(RCX, 17); a.sarRI(RCX, 17);
                    a.movRR(gp(dst), RCX); otype[dst] = VT_INT;
                } else {
                    a.movRR(RDX, RCX); a.movAbs(RAX, JIT_TAGS); a.andRR(RDX, RAX); a.cmpRR(RDX, RAX);
                    Label okF; a.jcc(NE, okF); bailTo(off, depth); a.bind(okF);
                    a.movqXR(xm(dst), RCX); otype[dst] = VT_NUM;
                }
                depth++; break;
            }
            case Op::NEGATE: {
                int t = otype[depth-1];
                if (t == VT_INT) {
                    a.movRR(RAX, gp(depth-1)); a.negR(RAX);        // -x
                    a.movRR(RCX, RAX); a.shlRI(RCX, 17); a.sarRI(RCX, 17); a.cmpRR(RCX, RAX);
                    Label okN; a.jcc(E, okN); bailTo(off, depth); a.bind(okN);   // overflow of inline range
                    a.movRR(gp(depth-1), RAX);
                } else if (t == VT_NUM) {
                    a.movqRX(RAX, xm(depth-1));
                    a.movAbs(RCX, 0x8000000000000000ull); a.xorRR(RAX, RCX);   // flip sign bit
                    a.movqXR(xm(depth-1), RAX);
                } else { ok = false; break; }
                break;
            }
            case Op::LIST: {
                // Small list literal -> allocate in native code via lovaxJitMakeList.
                // The n elements are boxed into a native-stack buffer; the caller-saved
                // registers the C call would clobber are spilled around it. gcAlloc
                // never collects mid-call, so no GC can run here — the trace's own
                // LOOP back-edge is the safepoint that collects, with the value stack
                // as roots. Frame (192B, 16-aligned): [0]=orig RSP, [8]=result,
                // [16..64)=6 caller-saved GP, [64..128)=XMM0-7, [128..192)=elem buffer.
                int n = (int)rdU16(off + 1);
                for (int k = 0; k < n; ++k) {
                    int t = otype[depth - n + k];
                    if (t == VT_STRUCT || t == VT_CALLEE) { ok = false; break; }
                }
                if (!ok) break;
                const int GP_OFF = 16, XMM_OFF = 64, BUF_OFF = 128, FRAME = 192;
                static const Reg savedGp[6] = { RSI, RDI, R8, R9, R10, R11 };
                a.movRR(RAX, RSP); a.andRI(RSP, -16); a.subRI(RSP, FRAME);
                a.movMR(RSP, 0, RAX);                              // save original RSP
                for (int k = 0; k < n; ++k) {                      // box elements (operands still live)
                    int slot = depth - n + k, t = otype[slot];
                    if (t == VT_NUM)      a.movsdMX(RSP, BUF_OFF + k * 8, xm(slot));
                    else if (t == VT_INT) { boxTo(RDX, gp(slot)); a.movMR(RSP, BUF_OFF + k * 8, RDX); }
                    else                  a.movMR(RSP, BUF_OFF + k * 8, gp(slot));   // VT_WORD/VT_OBJ raw
                }
                for (int k = 0; k < 6; ++k) a.movMR(RSP, GP_OFF + k * 8, savedGp[k]);
                for (int k = 0; k < 8; ++k) a.movsdMX(RSP, XMM_OFF + k * 8, (Xmm)k);
                a.movRR(RDI, RSP); a.addRI(RDI, BUF_OFF);          // arg0 = &buffer
                a.movAbs(RSI, (uint64_t)(int64_t)n);               // arg1 = n
                a.movAbs(RAX, (uint64_t)(uintptr_t)&Lovax::lovaxJitMakeList);
                a.callR(RAX);
                a.movMR(RSP, 8, RAX);                              // save boxed list word
                for (int k = 0; k < 8; ++k) a.movsdXM((Xmm)k, RSP, XMM_OFF + k * 8);
                for (int k = 0; k < 6; ++k) a.movRM(savedGp[k], RSP, GP_OFF + k * 8);
                a.movRM(RAX, RSP, 8);                              // result
                a.movRM(RSP, RSP, 0);                             // restore original RSP
                int res = depth - n;
                a.movRR(gp(res), RAX); otype[res] = VT_OBJ;
                depth += 1 - n;
                break;
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
            case Op::DIV: case Op::MOD: {
                // Floor division / modulo (Lovax spec: result takes the divisor's
                // sign). int/int -> idiv + a floor correction; a zero divisor bails
                // to the interpreter (which raises the error). INT_MIN/-1 can't
                // occur: an inline VT_INT is |x| < 2^46, so idiv never traps.
                if (!numOperand(depth-2) || !numOperand(depth-1)) { ok = false; break; }
                if (otype[depth-2] == VT_INT && otype[depth-1] == VT_INT) {
                    Reg lhs = gp(depth-2), rhs = gp(depth-1);
                    a.cmpRI(rhs, 0);
                    Label okD; a.jcc(NE, okD); bailTo(off, depth); a.bind(okD);   // divide/mod by zero
                    a.movRR(RAX, lhs); a.cqo(); a.idivR(rhs);        // RAX=quotient(trunc), RDX=remainder
                    // floor correction: if remainder != 0 and signs differ, adjust.
                    Label done;
                    a.cmpRI(RDX, 0); a.jcc(E, done);
                    a.movRR(RCX, lhs); a.xorRR(RCX, rhs); a.cmpRI(RCX, 0); a.jcc(GE, done);  // signs same -> skip
                    if (op == Op::DIV) a.subRI(RAX, 1);             // q-- toward -inf
                    else               a.addRR(RDX, rhs);           // rem += divisor
                    a.bind(done);
                    a.movRR(lhs, op == Op::DIV ? RAX : RDX);
                    otype[depth-2] = VT_INT;                        // |result| <= |lhs| < 2^46, in range
                } else if (op == Op::MOD) {
                    ok = false; break;                             // float modulo (fmod) not traced
                } else {
                    toX(depth-2); toX(depth-1);
                    // guard divisor != +/-0.0 via its bits (no XMM scratch): a value
                    // is +/-0.0 iff (bits << 1) == 0. NaN divisors don't match, so a
                    // bail only happens on a real zero (which the interpreter errors on).
                    a.movqRX(RAX, xm(depth-1)); a.shlRI(RAX, 1);
                    Label okZ; a.jcc(NE, okZ); bailTo(off, depth); a.bind(okZ);
                    a.divsd(xm(depth-2), xm(depth-1));
                    otype[depth-2] = VT_NUM;
                }
                depth--; break;
            }

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
                    if (ii->second == 5) {
                        // push(list, val): [callee, list, val]. Number value only
                        // (Part 1) — appends inline; a full backing side-exits so the
                        // interpreter grows it (so no reallocation ever happens in the
                        // trace, keeping any cached begin pointer valid). Returns the
                        // list. No write barrier is needed for a non-object value.
                        if (otype[depth-2] != VT_OBJ) { ok = false; break; }
                        int vt = otype[depth-1];
                        if (vt != VT_INT && vt != VT_NUM) { ok = false; break; }
                        a.movRR(RAX, gp(depth-2)); a.movAbs(RCX, JIT_PAYLOAD); a.andRR(RAX, RCX);  // ListObject*
                        a.movRM(RCX, RAX, VEC_FINISH_OFF);      // e_ (end)
                        a.movRM(RDX, RAX, VEC_CAP_OFF);         // c_ (cap)
                        a.cmpRR(RCX, RDX);
                        Label okCap; a.jcc(B, okCap); bailTo(off, depth); a.bind(okCap);   // full -> grow -> bail
                        a.movRR(RDX, RCX);                      // RDX = old e_ (store address)
                        a.addRI(RCX, 8); a.movMR(RAX, VEC_FINISH_OFF, RCX);   // e_ += 8
                        if (vt == VT_NUM) { a.movsdMX(RDX, 0, xm(depth-1)); }
                        else { boxTo(RAX, gp(depth-1)); a.movMR(RDX, 0, RAX); }   // RAX free after the e_ store
                        a.movRR(gp(calleeSlot), gp(depth-2)); otype[calleeSlot] = VT_OBJ;  // push returns the list
                        depth -= argc;
                        break;
                    }
                    if (ii->second == 4) {
                        // abs(x): type-dependent, so do NOT coerce to float first.
                        // float -> clear the sign bit in a GP register (no XMM scratch,
                        // so no live NUM cell is clobbered); matches std::fabs bit-for-bit
                        // incl -0.0/NaN/inf. int -> branchless |v| = (v^(v>>63))-(v>>63),
                        // and a VT_INT operand is always inline-range so |v| stays in range.
                        if (otype[depth - 1] == VT_NUM) {
                            a.movqRX(RAX, xm(depth - 1));
                            a.movAbs(RCX, 0x7FFFFFFFFFFFFFFFull); a.andRR(RAX, RCX);
                            a.movqXR(xm(calleeSlot), RAX);
                            otype[calleeSlot] = VT_NUM;
                        } else if (otype[depth - 1] == VT_INT) {
                            Reg s = gp(depth - 1);
                            a.movRR(RAX, s); a.sarRI(RAX, 63);              // sign mask: 0 or -1
                            a.movRR(RCX, s); a.xorRR(RCX, RAX); a.subRR(RCX, RAX);
                            a.movRR(gp(calleeSlot), RCX);
                            otype[calleeSlot] = VT_INT;
                        } else { ok = false; break; }        // non-number -> decline region
                        depth -= argc;
                        break;
                    }
                    toX(depth - 1);                          // arg -> xm(depth-1), a double
                    if (ii->second == 1) {
                        // sqrt(x): guard arg >= 0 (the builtin errors on a negative),
                        // then sqrtsd into the result slot (stays a float).
                        a.xorpd(XMM0, XMM0);                  // 0.0
                        a.ucomisd(xm(depth - 1), XMM0);
                        Label okp; a.jcc(AE, okp); bailTo(off, depth); a.bind(okp);  // arg<0 or NaN -> bail
                        a.sqrtsd(xm(calleeSlot), xm(depth - 1));
                        otype[calleeSlot] = VT_NUM;
                    } else {
                        // floor/ceil(x) -> int. roundsd to the integral double
                        // (imm 1=toward -inf, 2=toward +inf), then cvttsd2si — which
                        // is exactly the interpreter's (long long) cast on x86-64, so
                        // NaN/inf/out-of-range all land on the same indefinite value.
                        // Result into RAX first: a VT_INT in the trace must fit the
                        // 47-bit inline range (the invariant boxTo relies on), so
                        // range-check and bail BEFORE overwriting the callee slot —
                        // keeping the snapshot's callee word intact if we exit.
                        a.roundsd(xm(depth - 1), xm(depth - 1), ii->second == 3 ? 2 : 1);  // floor(2)->1 toward -inf, ceil(3)->2 toward +inf
                        a.cvttsd2si(RAX, xm(depth - 1));
                        a.movRR(RCX, RAX); a.shlRI(RCX, 17); a.sarRI(RCX, 17); a.cmpRR(RCX, RAX);
                        Label okR; a.jcc(E, okR); bailTo(off, depth); a.bind(okR);
                        a.movRR(gp(calleeSlot), RAX);
                        otype[calleeSlot] = VT_INT;
                    }
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

            case Op::CLOSE_UPVALUE: break;   // no-op: a traceable region creates no closures,
                                             // so nothing captured this slot to close.
            case Op::FOR_NEXT: {
                // Range for-loop head. The iterator lives on the VM stack just below
                // the region operands (at [R_SP-8]) and is invariant across the loop,
                // so it is read from memory each iteration (all L1 hits). Non-range
                // iterators and non-positive steps side-exit; the loop variable v1 is
                // a dirty local cell that this op assigns each iteration.
                int v1 = localCell[(int)rdU16(off + 2)];
                Reg v1reg = gpCell(v1);
                size_t exitOff = next + rdU16(off + 6);
                a.movRM(RAX, R_SP, -8);
                a.movAbs(RCX, JIT_PAYLOAD); a.andRR(RAX, RCX);          // RAX = IterObject*
                a.movzxRM8(RCX, RAX, ITER_KIND_OFF); a.cmpRI(RCX, ITER_KIND_RANGE);
                Label okK; a.jcc(E, okK); bailTo(off, depth); a.bind(okK);   // non-range -> interpreter
                a.movRM(RCX, RAX, ITER_SOURCE_OFF);                     // RCX = RangeObject*
                a.movRM(RDX, RCX, RANGE_STEP_OFF); a.cmpRI(RDX, 0);
                Label okS; a.jcc(G, okS); bailTo(off, depth); a.bind(okS);   // positive step only
                a.movRM(v1reg, RAX, ITER_INDEX_OFF);                   // v1 = cur = index
                a.movRM(RCX, RCX, RANGE_END_OFF);                      // RCX = end
                a.cmpRR(v1reg, RCX);
                Label okC; a.jcc(L, okC); bailTo(exitOff, depth); a.bind(okC);   // cur >= end -> done
                a.movRR(RCX, v1reg); a.addRR(RCX, RDX);
                a.movMR(RAX, ITER_INDEX_OFF, RCX);                     // index += step
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
