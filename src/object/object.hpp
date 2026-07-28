#ifndef OBJECT_HPP
#define OBJECT_HPP

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <unordered_map>
#include <sstream>
#include <chrono>
#include <deque>
#include <cstdint>

namespace Lovax {


enum class ObjectType {
    INTEGER,
    FLOAT,
    BOOLEAN,
    NULL_OBJ,
    STRING,
    LIST,
    TUPLE,
    MAP,
    SET,
    BYTES,
    COMPLEX,
    DEQUE,
    RANGE,
    FUNCTION,
    BUILTIN,
    SIGNAL,
    COROUTINE,
    STRUCT,
    RETURN_VALUE,
    BREAK_SIGNAL,
    CONTINUE_SIGNAL,
    ERROR,
    BOXED_INT,  // RFC-024: int64 outside the 47-bit inline range (NANBOX only)
    NATIVE      // RFC-025: opaque host handle (engine object behind a void*)
};

class Object {
public:
    // Non-virtual type tag: hot paths read this field instead of paying for a
    // virtual call. type() stays for readability and returns the same tag.
    const ObjectType tag;

    // GC bookkeeping (RFC-013/RFC-023). gcNext links every heap object;
    // gcColor is the tri-color state (0 white, 1 gray, 2 black) driving the
    // incremental collector. Touched by the collector and the write barrier.
    Object* gcNext = nullptr;
    unsigned char gcColor = 0;
    // Pool size-class of this object's allocation (RFC-024 pre-work): the sweep
    // returns the raw memory to the right free list. 0xFF = malloc'd (oversized).
    // Fits in the padding after gcColor, so it costs no extra bytes.
    unsigned char gcSizeClass = 0;

    explicit Object(ObjectType t) : tag(t) {}
    virtual ~Object() = default;
    ObjectType type() const { return tag; }
    virtual std::string inspect() const = 0;

    // Marks this object's outgoing references (children). Leaf objects (numbers,
    // strings, bools) have none. Container/closure types override this.
    virtual void gcMark() {}

    // Approximate heap footprint (header + owned payload). Drives the GC
    // threshold, so containers/strings override it — order-of-magnitude
    // accuracy is enough, but it must scale with the real payload.
    virtual size_t gcBytes() const { return 48; }
};

} // namespace Lovax — reopened below; gc.hpp needs Object complete
#include "../vm/gc.hpp"
namespace Lovax {

// Marks an object gray: queues it for child-scanning. Safe on null and on
// already-marked objects (the graph may have cycles).
inline void gcMarkObject(Object* o) {
    if (o == nullptr || o->gcColor != GC_WHITE) return;
    o->gcColor = GC_GRAY;
    Heap::get().worklist.push_back(o);
}

// Write barrier (RFC-023, Dijkstra "shade the child"): while marking is in
// progress, a reference written INTO a pre-existing container might attach a
// white object to an already-scanned (black) one — the marker would never see
// it. Shading the child gray closes the hole. Container color is not checked:
// over-shading is a little extra marking, never a bug.
inline void gcShade(Object* o) {
    if (Heap::get().phase == GcPhase::MARK) gcMarkObject(o);
}

namespace GcDetail {

// Marks every root: permanents (singletons/modules), the runtime's VM roots
// (stacks/globals/frames), and C++-held temporaries.
inline void markAllRoots(Heap& h) {
    for (Object* r : h.permanentRoots) gcMarkObject(r);
    if (h.markRoots) h.markRoots();
    for (Object* r : h.tempRoots) gcMarkObject(r);
    for (Object* r : h.hostRoots) gcMarkObject(r);   // embed host-held values (RFC-025)
}

// Processes up to `budget` gray objects. Returns true when the list drained.
inline bool markStep(Heap& h, size_t budget) {
    while (budget-- > 0) {
        if (h.worklist.empty()) return true;
        Object* o = h.worklist.back();
        h.worklist.pop_back();
        o->gcColor = GC_BLACK;
        o->gcMark();
    }
    return h.worklist.empty();
}

// Sweeps up to `budget` objects from the cursor. Returns true when done.
inline bool sweepStep(Heap& h, size_t budget) {
    while (budget-- > 0) {
        Object* o = *h.sweepCursor;
        if (o == nullptr) return true;
        if (o->gcColor != GC_WHITE) {
            o->gcColor = GC_WHITE;
            h.sweepLive += o->gcBytes();
            h.sweepCursor = &o->gcNext;
        } else {
            *h.sweepCursor = o->gcNext;
#if defined(LOVAX_GC_STRESS) || defined(LOVAX_GC_STRESS_INC)
            delete o;
#else
            // Destroy the object, then hand the raw slot back to its free list.
            unsigned char sc = o->gcSizeClass;
            o->~Object();
            h.pool.freeRaw(o, sc);
#endif
        }
    }
    return *h.sweepCursor == nullptr;
}

inline void finishCycle(Heap& h) {
    // Splice sweep-born objects back onto the main list (white, unscanned —
    // the next cycle treats them like any other object).
    if (h.newborn != nullptr) {
        h.newbornTail->gcNext = h.first;
        h.first = h.newborn;
        h.newborn = h.newbornTail = nullptr;
    }
    h.bytesAllocated = h.sweepLive + h.allocSinceSweep;
    h.nextGC = h.bytesAllocated + h.bytesAllocated / 2;   // grow 1.5×
    if (h.nextGC < 4 * 1024 * 1024) h.nextGC = 4 * 1024 * 1024;
    h.phase = GcPhase::IDLE;
    h.sweepCursor = nullptr;
    h.collections++;
}

} // namespace GcDetail

// One incremental slice (RFC-023): runs at safepoints only. Each slice stays
// within a small object budget so the pause is bounded (the game-frame
// contract); the cycle spreads across many safepoints.
inline void gcStep() {
    Heap& h = Heap::get();
    if (h.collecting) return;
    h.collecting = true;
    auto t0 = std::chrono::steady_clock::now();

    switch (h.phase) {
        case GcPhase::IDLE:
            // Begin a cycle: gray the roots (cheap — the root SET is small).
            h.phase = GcPhase::MARK;
            GcDetail::markAllRoots(h);
            break;
        case GcPhase::MARK:
            if (GcDetail::markStep(h, h.stepBudget)) {
                // Atomic finish: stacks are not write-barriered, so re-scan
                // every root and drain what that grays. Root sets are small,
                // and most of the graph is already black — this stays short.
                GcDetail::markAllRoots(h);
                while (!GcDetail::markStep(h, h.stepBudget * 4)) {}
                h.sweepCursor = &h.first;
                h.sweepLive = 0;
                h.allocSinceSweep = 0;
                h.phase = GcPhase::SWEEP;
            }
            break;
        case GcPhase::SWEEP:
            if (GcDetail::sweepStep(h, h.stepBudget * 2)) {
                GcDetail::finishCycle(h);
            }
            break;
    }

    auto dt = std::chrono::duration_cast<std::chrono::nanoseconds>(
                  std::chrono::steady_clock::now() - t0).count();
    h.gcNanos += dt;
    if (dt > h.maxPauseNanos) h.maxPauseNanos = dt;
    h.collecting = false;
}

// One full stop-the-world cycle (GC_STRESS and shutdown paths): all phases to
// completion in a single pause.
inline void gcCollect() {
    Heap& h = Heap::get();
    if (h.collecting) return;
    h.collecting = true;
    auto t0 = std::chrono::steady_clock::now();

    if (h.phase == GcPhase::IDLE) {
        h.phase = GcPhase::MARK;
        GcDetail::markAllRoots(h);
    }
    if (h.phase == GcPhase::MARK) {
        while (!GcDetail::markStep(h, 4096)) {}
        GcDetail::markAllRoots(h);
        while (!GcDetail::markStep(h, 4096)) {}
        h.sweepCursor = &h.first;
        h.sweepLive = 0;
        h.allocSinceSweep = 0;
        h.phase = GcPhase::SWEEP;
    }
    while (!GcDetail::sweepStep(h, 65536)) {}
    GcDetail::finishCycle(h);

    auto dt = std::chrono::duration_cast<std::chrono::nanoseconds>(
                  std::chrono::steady_clock::now() - t0).count();
    h.gcNanos += dt;
    if (dt > h.maxPauseNanos) h.maxPauseNanos = dt;
    h.collecting = false;
}

// ===== Defined int64 arithmetic =====
// Signed overflow is UB in C++; the LANGUAGE's spec is two's-complement wrap —
// identical bits on every platform (the determinism promise) and UBSan-clean.
// Also closes a real crash: INT64_MIN / -1 and % -1 raise SIGFPE on x86 —
// callers guard with the -1 special case (divide: wrapNeg; modulo: 0).
inline long long wrapAdd(long long a, long long b) {
    return (long long)((unsigned long long)a + (unsigned long long)b);
}
inline long long wrapSub(long long a, long long b) {
    return (long long)((unsigned long long)a - (unsigned long long)b);
}
inline long long wrapMul(long long a, long long b) {
    return (long long)((unsigned long long)a * (unsigned long long)b);
}
inline long long wrapNeg(long long a) {
    return (long long)(0ull - (unsigned long long)a);
}
inline long long wrapShl(long long a, long long s) {
    return (long long)((unsigned long long)a << s);
}

// Human-friendly float printing: 6.28 (not 6.280000), 2.0 (not 2 — keep the type visible)
inline std::string formatFloat(double v) {
    // One canonical NaN, no sign: "-nan" is a libc/platform accident, and the
    // NANBOX layout (RFC-024) canonicalizes NaN bits anyway — both layouts and
    // all platforms must print the same thing (the determinism promise).
    if (v != v) return "nan";
    std::ostringstream ss;
    ss.precision(14);
    ss << v;
    std::string s = ss.str();
    // "inf", "nan" gibi özel durumlar olduğu gibi kalır
    if (s.find('.') == std::string::npos &&
        s.find('e') == std::string::npos &&
        s.find("inf") == std::string::npos &&
        s.find("nan") == std::string::npos) {
        s += ".0";
    }
    return s;
}

class IntegerObject : public Object {
public:
    long long value;
    IntegerObject(long long val) : Object(ObjectType::INTEGER), value(val) {}
    std::string inspect() const override { return std::to_string(value); }
};

class FloatObject : public Object {
public:
    double value;
    FloatObject(double val) : Object(ObjectType::FLOAT), value(val) {}
    std::string inspect() const override { return formatFloat(value); }
};

// RFC-024 (NANBOX): carrier for an int64 outside [-2^46, 2^46). Lives ONLY
// inside a Value — every bridge into the Object world (toObject) converts it
// to a plain IntegerObject, so no other subsystem ever sees this type. To the
// language it IS an int: Value::tag()/asInt()/kind() treat it transparently.
class BoxedIntObject : public Object {
public:
    long long value;
    BoxedIntObject(long long v) : Object(ObjectType::BOXED_INT), value(v) {}
    std::string inspect() const override { return std::to_string(value); }
};

class StringObject : public Object {
public:
    std::string value;
    // Cached UTF-8 code-point count; -1 = not computed. Strings are immutable
    // except for the VM's in-place append (ADD_INPLACE), which resets it.
    mutable long long lenCache = -1;
    StringObject(const std::string& val) : Object(ObjectType::STRING), value(val) {}
    std::string inspect() const override { return value; } // Prints the raw text on the console
    size_t gcBytes() const override { return sizeof(*this) + value.capacity(); }
};

class BooleanObject : public Object {
public:
    bool value;
    BooleanObject(bool val) : Object(ObjectType::BOOLEAN), value(val) {}
    std::string inspect() const override { return value ? "true" : "false"; }
};

class NullObject : public Object {
public:
    NullObject() : Object(ObjectType::NULL_OBJ) {}
    std::string inspect() const override { return "null"; }
};

// ===== Shared constants (singletons) =====
// Single instances so true/false/null never allocate new objects.
inline const Ref<BooleanObject> TRUE_OBJ  = makeObj<BooleanObject>(true);
inline const Ref<BooleanObject> FALSE_OBJ = makeObj<BooleanObject>(false);
inline const Ref<NullObject>    NULL_OBJ_ = makeObj<NullObject>();

inline Ref<BooleanObject> boolObj(bool b) { return b ? TRUE_OBJ : FALSE_OBJ; }

// Shows strings quoted inside nested structures: say ["a"] -> ["a"]
inline std::string inspectQuoted(const Ref<Object>& obj) {
    if (obj->type() == ObjectType::STRING) {
        return "\"" + obj->inspect() + "\"";
    }
    return obj->inspect();
}

// ===== The VM value type (RFC-024) — moved here (ahead of the containers) so
// ListObject/TupleObject can store elements UNBOXED as std::vector<Value>: an
// inline int/float/bool rides in the 8-byte word instead of being a separate
// heap object per element. Dependencies (Object, BoxedIntObject, the scalar
// objects, Ref/gcAlloc) are all complete above this point. The free helpers
// (toObject/valueEquals/valueTypeName) stay in value.hpp — they need
// objectEquals/typeName, defined further down. =====

enum class VKind : uint8_t { NIL, BOOL, INT, FLOAT, OBJ };

// The 8-byte NaN-boxed layout is the default on 64-bit targets where the
// 47-bit user-VA assumption is verified (RFC-024 gate: every golden bit-for-bit
// incl. the int64 proof, ASan-clean under both GC stress modes, and measured
// equal-or-faster on every bench — fib −16%, btree −8%). Define LOVAX_NO_NANBOX
// to force the portable 16-byte union; other architectures get it automatically.
#if !defined(LOVAX_NO_NANBOX) && !defined(LOVAX_NANBOX) && \
    (defined(__x86_64__) || defined(_M_X64) || defined(__aarch64__) || defined(_M_ARM64))
#define LOVAX_NANBOX 1
#endif
#ifdef LOVAX_NO_NANBOX
#undef LOVAX_NANBOX
#endif

#ifdef LOVAX_NANBOX

// 8-byte NaN-boxed value (RFC-024). A double is stored as its own bit pattern
// — every double WRITE canonicalizes NaN to the positive quiet NaN, so no
// real double can collide with the tag range (x86 0/0 yields the NEGATIVE
// quiet NaN 0xFFF8..., which IS in the range — canonicalization is what makes
// the scheme sound). Everything else: top 13 bits set + 4-bit tag + 47-bit
// payload. Integers in [-2^46, 2^46) ride inline; rarer int64s are heap-boxed
// (BoxedIntObject) so exact int64 semantics survive — the hybrid that LuaJIT's
// own scheme (int32-or-double) does not offer. Layout study: lj_obj.h (credit,
// not copy); V8 smi; JSC JSValue.
struct Value {
private:
    static constexpr uint64_t TAGS      = 0xFFF8000000000000ull; // top 13 bits
    static constexpr uint64_t PAYLOAD   = 0x00007FFFFFFFFFFFull; // low 47 bits
    static constexpr int      TAG_SHIFT = 47;
    // T_BOXINT gets its OWN tag (not a subtype of T_OBJ): every tag()/isObj()/
    // isInt() query stays pointer-deref-free — only an actual asInt() read of a
    // boxed value touches the heap. (Sharing T_OBJ cost a deref per type query
    // on object-heavy code: measured +25% on heavy_loop.)
    static constexpr uint64_t T_NIL = 1, T_FALSE = 2, T_TRUE = 3,
                              T_INT = 4, T_OBJ = 5, T_BOXINT = 6;
    static constexpr uint64_t TAG_MASK = TAGS | (0xFull << TAG_SHIFT);
    static constexpr uint64_t mk(uint64_t t) { return TAGS | (t << TAG_SHIFT); }

    uint64_t u_ = mk(T_NIL);

    static uint64_t fromDouble(double x) {
        if (x != x) return 0x7FF8000000000000ull;   // canonical quiet NaN
        uint64_t r; std::memcpy(&r, &x, 8); return r;
    }
    static double toDouble(uint64_t u) { double d; std::memcpy(&d, &u, 8); return d; }
    static bool fitsInline(long long x) {
        return x >= -(1ll << 46) && x < (1ll << 46);
    }
    bool tagged() const { return (u_ & TAGS) == TAGS; }
    uint64_t tbits() const { return u_ & TAG_MASK; }
    Object* ptr() const { return reinterpret_cast<Object*>(u_ & PAYLOAD); }

public:
    Value() = default;
    static Value nil()           { Value v; v.u_ = mk(T_NIL); return v; }
    static Value boolean(bool x) { Value v; v.u_ = mk(x ? T_TRUE : T_FALSE); return v; }
    static Value integer(long long x) {
        Value v;
        if (fitsInline(x)) {
            v.u_ = mk(T_INT) | ((uint64_t)x & PAYLOAD);
        } else {
            v.u_ = mk(T_BOXINT) |
                   (reinterpret_cast<uint64_t>(gcAlloc<BoxedIntObject>(x)) & PAYLOAD);
        }
        return v;
    }
    static Value real(double x)  { Value v; v.u_ = fromDouble(x); return v; }
    static Value object(Ref<Object> o) {
        Value v;
        v.u_ = mk(T_OBJ) | (reinterpret_cast<uint64_t>(o.get()) & PAYLOAD);
        return v;
    }

    bool isNil()   const { return tbits() == mk(T_NIL); }
    bool isBool()  const { return tbits() == mk(T_TRUE) || tbits() == mk(T_FALSE); }
    bool isInt()   const { return tbits() == mk(T_INT) || tbits() == mk(T_BOXINT); }
    bool isFloat() const { return !tagged(); }
    bool isObj()   const { return tbits() == mk(T_OBJ); }
    bool isNumber() const { return isFloat() || isInt(); }

    double asDouble() const { return isInt() ? (double)asInt() : asFloat(); }

    bool isObjType(ObjectType t) const {
        return tbits() == mk(T_OBJ) && ptr() && ptr()->tag == t;
    }

    // Same accessor API as the 16-byte layout — call sites are identical.
    // No branch here touches the heap: T_BOXINT is its own tag.
    VKind tag() const {
        if (!tagged()) return VKind::FLOAT;
        switch ((u_ >> TAG_SHIFT) & 0xF) {
            case T_NIL:   return VKind::NIL;
            case T_TRUE: case T_FALSE: return VKind::BOOL;
            case T_INT: case T_BOXINT: return VKind::INT;
            default:      return VKind::OBJ;
        }
    }
    long long asInt() const {
        if (tbits() == mk(T_INT)) return (long long)(u_ << 17) >> 17; // sign-extend bit 46
        return static_cast<BoxedIntObject*>(ptr())->value;
    }
    double  asFloat() const { return toDouble(u_); }
    bool    asBool() const  { return tbits() == mk(T_TRUE); }
    Object* asObj() const   { return ptr(); }
    void setInt(long long x)  { *this = integer(x); }   // may box (gcAlloc defers GC)
    void setFloat(double x)   { u_ = fromDouble(x); }
    void setBool(bool x)      { u_ = mk(x ? T_TRUE : T_FALSE); }
    void setObjPtr(Object* o) { u_ = mk(T_OBJ) | (reinterpret_cast<uint64_t>(o) & PAYLOAD); }
    void setNil()             { u_ = mk(T_NIL); }
    // Slot is dead; storing nil drops the reference just as well here.
    void wipeObj()            { u_ = mk(T_NIL); }

    // GC-facing raw view: TRUE for any carried pointer INCLUDING a boxed int
    // (isObj() hides the box from the language; the collector must not miss it).
    bool hasPtr() const {
        return (tbits() == mk(T_OBJ) || tbits() == mk(T_BOXINT)) && ptr() != nullptr;
    }
    Object* rawPtr() const { return ptr(); }
};

#else // ---------------- 16-byte tagged union (portable fallback) ----------

// 16-byte tagged value (matches Lua 5.4, which also keeps a native int64).
// The default on any platform where the NANBOX 47-bit-pointer assumption is
// not verified; behind the same accessor API the two layouts are drop-in
// interchangeable (RFC-024).
struct Value {
private:
    // RFC-024 Phase 1: the layout is private — ALL access goes through the
    // accessors below, so the 8-byte NANBOX layout can replace this block
    // without touching a single call site.
    VKind kind = VKind::NIL;
    union {
        bool b;
        long long i;
        double d;
        Object* obj;   // engaged only when kind == OBJ; lifetime managed by the GC
    };

public:
    Value() : kind(VKind::NIL), i(0) {}
    static Value nil()                { Value v; v.kind = VKind::NIL;   v.i = 0; return v; }
    static Value boolean(bool x)      { Value v; v.kind = VKind::BOOL;  v.b = x; return v; }
    static Value integer(long long x) { Value v; v.kind = VKind::INT;   v.i = x; return v; }
    static Value real(double x)       { Value v; v.kind = VKind::FLOAT; v.d = x; return v; }
    static Value object(Ref<Object> o) {
        Value v; v.kind = VKind::OBJ; v.obj = o.get(); return v;
    }

    bool isNil()   const { return kind == VKind::NIL; }
    bool isBool()  const { return kind == VKind::BOOL; }
    bool isInt()   const { return kind == VKind::INT; }
    bool isFloat() const { return kind == VKind::FLOAT; }
    bool isObj()   const { return kind == VKind::OBJ; }
    bool isNumber() const { return kind == VKind::INT || kind == VKind::FLOAT; }

    double asDouble() const { return kind == VKind::INT ? (double)i : d; }

    bool isObjType(ObjectType t) const { return kind == VKind::OBJ && obj->type() == t; }

    // ---- RFC-024 Phase 1 accessors: the only sanctioned access paths. ----
    // The raw fields stay public until every site is converted; then they go
    // private and the 8-byte NANBOX layout slots in behind this exact API
    // (tag test = one compare, payload = shift/mask — no call sites change).
    VKind tag() const           { return kind; }
    long long asInt() const     { return i; }     // valid only when isInt()
    double    asFloat() const   { return d; }     // valid only when isFloat()
    bool      asBool() const    { return b; }     // valid only when isBool()
    Object*   asObj() const     { return obj; }   // valid only when isObj()
    void setInt(long long x)    { kind = VKind::INT;   i = x; }
    void setFloat(double x)     { kind = VKind::FLOAT; d = x; }
    void setBool(bool x)        { kind = VKind::BOOL;  b = x; }
    void setObjPtr(Object* o)   { kind = VKind::OBJ;   obj = o; }
    void setNil()               { kind = VKind::NIL;   i = 0; }
    // Stack-hygiene helper: null the reference WITHOUT retagging (used when a
    // popped slot is wiped so the GC root scan can't see a stale pointer).
    // Under NANBOX this becomes "store nil" — the slot is dead either way.
    void wipeObj()              { obj = nullptr; }

    // GC-facing raw view (parity with the NANBOX layout; here isObj == hasPtr
    // modulo the wiped-slot null).
    bool hasPtr() const { return kind == VKind::OBJ && obj != nullptr; }
    Object* rawPtr() const { return obj; }
};

#endif // LOVAX_NANBOX

// Value-level GC hooks + display, defined here (not value.hpp) because the
// container methods below (ListObject::gcMark/inspect) need them. They depend
// only on things already complete above.
inline void gcMarkValue(const Value& v)  { if (v.hasPtr()) gcMarkObject(v.rawPtr()); }
inline void gcShadeValue(const Value& v) { if (v.hasPtr()) gcShade(v.rawPtr()); }

inline std::string valueInspect(const Value& v) {
    switch (v.tag()) {
        case VKind::NIL:   return "null";
        case VKind::BOOL:  return v.asBool() ? "true" : "false";
        case VKind::INT:   return std::to_string(v.asInt());
        case VKind::FLOAT: return formatFloat(v.asFloat());
        case VKind::OBJ:   return v.asObj()->inspect();
    }
    return "";
}
// Quoted form for nested display: say ["a"] -> ["a"]
inline std::string valueInspectQuoted(const Value& v) {
    if (v.isObj() && v.asObj()->type() == ObjectType::STRING)
        return "\"" + v.asObj()->inspect() + "\"";
    return valueInspect(v);
}

// List object -> [1, 2, "three"]. Elements are UNBOXED Values (RFC: unboxed
// list storage) — an int/float/bool element is the inline 8-byte word, not a
// per-element heap object. This is the array fast path (qsort/sieve/index).
class ListObject : public Object {
public:
    ListObject() : Object(ObjectType::LIST) {}
    std::vector<Value> elements;
protected:
    explicit ListObject(ObjectType t) : Object(t) {}   // for TupleObject
public:
    void gcMark() override { for (auto& e : elements) gcMarkValue(e); }
    size_t gcBytes() const override {
        return sizeof(*this) + elements.capacity() * sizeof(Value);
    }
    std::string inspect() const override {
        std::string out = "[";
        for (size_t i = 0; i < elements.size(); ++i) {
            if (i > 0) out += ", ";
            out += valueInspectQuoted(elements[i]);
        }
        return out + "]";
    }
};

// Tuple -> (1, 2): an immutable list. Shares ListObject's layout so every
// element-walking path (iteration, unpack, len) reuses the list code; the
// TUPLE tag is what blocks mutation (push/insert/index-set reject it).
class TupleObject : public ListObject {
public:
    TupleObject() : ListObject(ObjectType::TUPLE) {}
    std::string inspect() const override {
        std::string out = "(";
        for (size_t i = 0; i < elements.size(); ++i) {
            if (i > 0) out += ", ";
            out += valueInspectQuoted(elements[i]);
        }
        if (elements.size() == 1) out += ",";   // (5,) — Python's one-tuple mark
        return out + ")";
    }
};

// Map object -> {"name": "Kai"} — insertion order preserved (deterministic output)
// Modules are frozen maps too: use math -> math.lerp (RFC-006)
class MapObject : public Object {
public:
    MapObject() : Object(ObjectType::MAP) {}
protected:
    explicit MapObject(ObjectType t) : Object(t) {}   // for SetObject
public:
    std::vector<std::pair<Ref<Object>, Ref<Object>>> entries;
    // Key index (v0.17 Track A): ONE flat open-addressing table replaces the two
    // std::unordered_map typed indexes. A slot caches the key's 64-bit hash and
    // its entry position; a lookup is mask -> linear probe -> compare cached hash
    // -> confirm against the real key in `entries` (tag + value). No per-node
    // allocation, no key string copy, contiguous probing (lj_tab.c-inspired,
    // adapted to our separate ordered-entries design). Bool keys keep their
    // trivial 2-slot array.
    struct KeySlot { uint64_t h; size_t pos; };
    std::vector<KeySlot> index;     // capacity always a power of two; pos==NPOS empty
    size_t indexUsed = 0;           // filled slots (load kept under 70%)
    static constexpr size_t NPOS = (size_t)-1;
    size_t boolIndex[2] = {NPOS, NPOS};
    bool frozen = false;          // true: immutable (modules)
    std::string moduleName;       // module name if this is a module (for errors + inspect)

    // Hashes cover EVERY byte (a sampled hash goes quadratic on adversarial
    // keys — LuaJIT #555), then avalanche so power-of-two masking sees all bits.
    static uint64_t mix64(uint64_t h) {
        h ^= h >> 32; h *= 0xd6e8feb86659fd93ull; h ^= h >> 32;
        return h;
    }
    static uint64_t strHash(const char* s, size_t n) {
        uint64_t h = 1469598103934665603ull;              // FNV-1a 64
        for (size_t i = 0; i < n; ++i) { h ^= (unsigned char)s[i]; h *= 1099511628211ull; }
        return mix64(h);
    }
    static uint64_t intHash(long long v) {
        return mix64((uint64_t)v * 0x9E3779B97F4A7C15ull);
    }

    // Probes for (h, eq). Returns the matching slot, or the first empty slot
    // (the insertion point) if absent. Callers guarantee a non-full table.
    template <class EQ>
    size_t probeSlot(uint64_t h, EQ&& eq) const {
        size_t mask = index.size() - 1;
        size_t i = (size_t)h & mask;
        while (true) {
            const KeySlot& s = index[i];
            if (s.pos == NPOS) return i;
            if (s.h == h && eq(entries[s.pos].first.get())) return i;
            i = (i + 1) & mask;
        }
    }
    // Doubles the table (or creates the first 8 slots) and re-seats every slot
    // by its cached hash — entry keys are never re-hashed.
    void indexGrow() {
        size_t ncap = index.empty() ? 8 : index.size() * 2;
        std::vector<KeySlot> old = std::move(index);
        index.assign(ncap, KeySlot{0, NPOS});
        size_t mask = ncap - 1;
        for (const KeySlot& s : old) {
            if (s.pos == NPOS) continue;
            size_t i = (size_t)s.h & mask;
            while (index[i].pos != NPOS) i = (i + 1) & mask;
            index[i] = s;
        }
    }
    bool indexWantsGrow() const { return (indexUsed + 1) * 10 >= index.size() * 7; }
    // Rebuilds index + boolIndex from `entries` (after a removal shifted positions).
    void rebuildIndex() {
        size_t need = entries.size() * 10 / 7 + 1;
        size_t ncap = 8;
        while (ncap < need) ncap *= 2;
        index.assign(ncap, KeySlot{0, NPOS});
        indexUsed = 0;
        boolIndex[0] = boolIndex[1] = NPOS;
        size_t mask = ncap - 1;
        for (size_t p = 0; p < entries.size(); ++p) {
            Object* k = entries[p].first.get();
            uint64_t h;
            if (k->tag == ObjectType::STRING) {
                const std::string& s = static_cast<StringObject*>(k)->value;
                h = strHash(s.data(), s.size());
            } else if (k->tag == ObjectType::INTEGER) {
                h = intHash(static_cast<IntegerObject*>(k)->value);
            } else {  // BOOLEAN
                boolIndex[static_cast<BooleanObject*>(k)->value ? 1 : 0] = p;
                continue;
            }
            size_t i = (size_t)h & mask;
            while (index[i].pos != NPOS) i = (i + 1) & mask;   // keys are unique
            index[i] = KeySlot{h, p};
            indexUsed++;
        }
    }

    void gcMark() override {
        for (auto& e : entries) { gcMarkObject(e.first.get()); gcMarkObject(e.second.get()); }
    }
    size_t gcBytes() const override {
        return sizeof(*this) +
               entries.capacity() * sizeof(entries[0]) +
               index.capacity() * sizeof(KeySlot);
    }

    void copyIndexFrom(const MapObject& src) {
        index = src.index;
        indexUsed = src.indexUsed;
        boolIndex[0] = src.boolIndex[0];
        boolIndex[1] = src.boolIndex[1];
    }
    void clearIndex() {
        index.clear();
        indexUsed = 0;
        boolIndex[0] = boolIndex[1] = NPOS;
    }

    // Zero-allocation lookups by native key
    size_t findStr(const std::string& k) const {
        if (index.empty()) return NPOS;
        uint64_t h = strHash(k.data(), k.size());
        size_t i = probeSlot(h, [&](const Object* key) {
            return key->tag == ObjectType::STRING &&
                   static_cast<const StringObject*>(key)->value == k;
        });
        return index[i].pos;                      // NPOS when the probe hit empty
    }
    Ref<Object> getStr(const std::string& k) const {
        size_t pos = findStr(k);
        return pos == NPOS ? nullptr : entries[pos].second;
    }
    void setStr(const Ref<Object>& key, const std::string& k,
                const Ref<Object>& val) {
        gcShade(key.get()); gcShade(val.get());   // write barrier (RFC-023)
        if (indexWantsGrow()) indexGrow();
        uint64_t h = strHash(k.data(), k.size());
        size_t i = probeSlot(h, [&](const Object* kk) {
            return kk->tag == ObjectType::STRING &&
                   static_cast<const StringObject*>(kk)->value == k;
        });
        if (index[i].pos != NPOS) { entries[index[i].pos].second = val; return; }
        index[i] = KeySlot{h, entries.size()};
        indexUsed++;
        entries.push_back({key, val});
    }

    void set(const Ref<Object>& key, const Ref<Object>& val) {
        gcShade(key.get()); gcShade(val.get());   // write barrier (RFC-023)
        switch (key->type()) {
            case ObjectType::STRING:
                setStr(key, static_cast<StringObject*>(key.get())->value, val);
                return;
            case ObjectType::INTEGER: {
                long long k = static_cast<IntegerObject*>(key.get())->value;
                if (indexWantsGrow()) indexGrow();
                uint64_t h = intHash(k);
                size_t i = probeSlot(h, [&](const Object* kk) {
                    return kk->tag == ObjectType::INTEGER &&
                           static_cast<const IntegerObject*>(kk)->value == k;
                });
                if (index[i].pos != NPOS) { entries[index[i].pos].second = val; return; }
                index[i] = KeySlot{h, entries.size()};
                indexUsed++;
                entries.push_back({key, val});
                return;
            }
            case ObjectType::BOOLEAN: {
                int k = static_cast<BooleanObject*>(key.get())->value ? 1 : 0;
                if (boolIndex[k] != NPOS) { entries[boolIndex[k]].second = val; return; }
                boolIndex[k] = entries.size();
                entries.push_back({key, val});
                return;
            }
            default: return; // unsupported key type (caller raises the error)
        }
    }

    Ref<Object> get(const Ref<Object>& key) const {
        switch (key->type()) {
            case ObjectType::STRING:
                return getStr(static_cast<StringObject*>(key.get())->value);
            case ObjectType::INTEGER: {
                if (index.empty()) return nullptr;
                long long k = static_cast<IntegerObject*>(key.get())->value;
                size_t i = probeSlot(intHash(k), [&](const Object* kk) {
                    return kk->tag == ObjectType::INTEGER &&
                           static_cast<const IntegerObject*>(kk)->value == k;
                });
                size_t pos = index[i].pos;
                return pos == NPOS ? nullptr : entries[pos].second;
            }
            case ObjectType::BOOLEAN: {
                size_t pos = boolIndex[static_cast<BooleanObject*>(key.get())->value ? 1 : 0];
                return pos == NPOS ? nullptr : entries[pos].second;
            }
            default: return nullptr;
        }
    }

    bool remove(const Ref<Object>& key) {
        size_t pos = NPOS;
        switch (key->type()) {
            case ObjectType::STRING:
                pos = findStr(static_cast<StringObject*>(key.get())->value);
                break;
            case ObjectType::INTEGER: {
                if (index.empty()) return false;
                long long k = static_cast<IntegerObject*>(key.get())->value;
                size_t i = probeSlot(intHash(k), [&](const Object* kk) {
                    return kk->tag == ObjectType::INTEGER &&
                           static_cast<const IntegerObject*>(kk)->value == k;
                });
                pos = index[i].pos;
                break;
            }
            case ObjectType::BOOLEAN:
                pos = boolIndex[static_cast<BooleanObject*>(key.get())->value ? 1 : 0];
                break;
            default: return false;
        }
        if (pos == NPOS) return false;
        entries.erase(entries.begin() + pos);
        // Every stored position after the erased entry shifted down by one, and
        // the matching slot must vanish; a rebuild does both (removal was
        // already O(n) here — the erase itself shifts the entries vector).
        rebuildIndex();
        return true;
    }
    std::string inspect() const override {
        if (!moduleName.empty()) {
            return "<module " + moduleName + ": " + std::to_string(entries.size()) + " items>";
        }
        std::string out = "{";
        for (size_t i = 0; i < entries.size(); ++i) {
            if (i > 0) out += ", ";
            out += inspectQuoted(entries[i].first) + ": " + inspectQuoted(entries[i].second);
        }
        return out + "}";
    }
};

// Set -> Set{1, 2, "a"}: unique elements (string/int/bool), insertion-ordered.
// Inherits MapObject so the typed indexes, lookup, removal and key-snapshot
// iteration are all reused — a set is a value-less map (values are TRUE).
class SetObject : public MapObject {
public:
    SetObject() : MapObject(ObjectType::SET) {}
    std::string inspect() const override {
        std::string out = "Set{";
        for (size_t i = 0; i < entries.size(); ++i) {
            if (i > 0) out += ", ";
            out += inspectQuoted(entries[i].first);
        }
        return out + "}";
    }
};

// Immutable byte string -> b"\x01ab": binary data (files, sockets, future
// hash/compress modules). Raw std::string storage; index -> int, text() decodes.
class BytesObject : public Object {
public:
    std::string data;
    BytesObject() : Object(ObjectType::BYTES) {}
    explicit BytesObject(std::string d) : Object(ObjectType::BYTES), data(std::move(d)) {}
    size_t gcBytes() const override { return sizeof(*this) + data.capacity(); }
    std::string inspect() const override {
        std::string out = "b\"";
        char buf[8];
        for (unsigned char c : data) {
            if (c == '"' || c == '\\') { out += '\\'; out += (char)c; }
            else if (c >= 0x20 && c < 0x7f) out += (char)c;
            else { std::snprintf(buf, sizeof(buf), "\\x%02x", c); out += buf; }
        }
        return out + "\"";
    }
};

// Complex number -> 3 + 4j (heap object: two doubles cannot fit the 16-byte
// tagged Value). Arithmetic lives in Runtime::evalInfixExpression.
class ComplexObject : public Object {
public:
    double re = 0.0, im = 0.0;
    ComplexObject(double r, double i) : Object(ObjectType::COMPLEX), re(r), im(i) {}
    // Python-style: integral parts print without the trailing .0 -> (3+4j)
    static std::string fmtPart(double d) {
        if (d == (long long)d && d > -1e15 && d < 1e15) return std::to_string((long long)d);
        return formatFloat(d);
    }
    std::string inspect() const override {
        if (re == 0.0) return fmtPart(im) + "j";
        return "(" + fmtPart(re) + (im < 0 ? "-" : "+") + fmtPart(im < 0 ? -im : im) + "j)";
    }
};

// Opaque host handle (RFC-025 embed/FFI). Wraps a native pointer the engine
// owns — a Sprite*, an Entity, a file handle — so a script can hold it, pass it
// back to native functions, and store it in containers, without the VM knowing
// its shape. `typeId` lets a native callback verify the handle is the kind it
// expects; `typeName` is what kind()/inspect show.
//
// Lifetime, two modes:
//  - OWNING  (finalize != null): the GC calls finalize(ptr) when the handle is
//    collected — the engine frees its C++ object. finalize runs DURING the
//    sweep: it must NOT allocate a Lovax object or re-enter the VM (Lua __gc
//    rule). Use it only to hand `ptr` back to a plain C++ deleter.
//  - BORROWED (finalize == null): the engine owns the lifetime; the handle is a
//    non-owning view. The engine must not let the script outlive `ptr` (or use
//    a generation/validity scheme on its side).
class NativeObject : public Object {
public:
    void* ptr = nullptr;
    uint32_t typeId = 0;
    void (*finalize)(void*) = nullptr;
    const char* typeName = "native";   // static string (host-owned literal)

    NativeObject(void* p, uint32_t tid, void (*fin)(void*), const char* tn)
        : Object(ObjectType::NATIVE), ptr(p), typeId(tid),
          finalize(fin), typeName(tn ? tn : "native") {}
    ~NativeObject() override { if (finalize && ptr) finalize(ptr); }
    std::string inspect() const override {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%p", ptr);
        return std::string("<") + typeName + " " + buf + ">";
    }
};

// Double-ended queue -> collections.deque(): O(1) push/pop at both ends
// (a list pays O(n) for front operations). Module functions operate on it.
class DequeObject : public Object {
public:
    std::deque<Ref<Object>> items;
    DequeObject() : Object(ObjectType::DEQUE) {}
    void gcMark() override { for (auto& e : items) gcMarkObject(e.get()); }
    size_t gcBytes() const override { return sizeof(*this) + items.size() * 16; }
    std::string inspect() const override {
        std::string out = "deque[";
        for (size_t i = 0; i < items.size(); ++i) {
            if (i > 0) out += ", ";
            out += inspectQuoted(items[i]);
        }
        return out + "]";
    }
};

// Shared per struct TYPE (RFC-017): field->slot mapping and the method table,
// built once when the declaration executes. Instances point here instead of
// carrying keys, hash indexes and method entries per object.
class StructShapeObject : public Object {
public:
    std::string name;                                    // struct type name
    std::vector<Ref<StringObject>> fieldNames;           // slot -> key (keys()/inspect)
    std::unordered_map<std::string, int> fieldIndex;     // field name -> slot
    std::vector<std::pair<Ref<StringObject>, Ref<Object>>> methods; // declaration order
    std::unordered_map<std::string, size_t> methodIndex;

    StructShapeObject() : Object(ObjectType::RETURN_VALUE) {} // internal, never user-visible
    Ref<Object> getMethod(const std::string& n) const {
        auto it = methodIndex.find(n);
        return it == methodIndex.end() ? nullptr : methods[it->second].second;
    }
    void gcMark() override {
        for (auto& f : fieldNames) gcMarkObject(f.get());
        for (auto& m : methods) { gcMarkObject(m.first.get()); gcMarkObject(m.second.get()); }
    }
    std::string inspect() const override { return "<struct " + name + ">"; }
};

// A struct instance: shape pointer + flat slot array. Field names resolve to
// slot indexes through the shared shape, so per-instance cost is just the values.
class StructInstanceObject : public Object {
public:
    StructShapeObject* shape = nullptr;   // kept alive by gcMark below
    std::vector<Ref<Object>> slots;

    StructInstanceObject() : Object(ObjectType::STRUCT) {}
    Ref<Object> getField(const std::string& n) const {
        auto it = shape->fieldIndex.find(n);
        return it == shape->fieldIndex.end() ? nullptr : slots[it->second];
    }
    void gcMark() override {
        gcMarkObject(shape);
        for (auto& s : slots) gcMarkObject(s.get());
    }
    size_t gcBytes() const override {
        return sizeof(*this) + slots.capacity() * sizeof(Ref<Object>);
    }
    std::string inspect() const override {
        std::string out = "{__type__: " + shape->name;
        for (size_t i = 0; i < slots.size(); ++i) {
            out += ", " + shape->fieldNames[i]->value + ": " + inspectQuoted(slots[i]);
        }
        return out + "}";
    }
};

// Lazy range object -> range(0, 10): never allocates a million-element list
class RangeObject : public Object {
public:
    long long start, end, step;
    RangeObject(long long s, long long e, long long st)
        : Object(ObjectType::RANGE), start(s), end(e), step(st) {}

    long long length() const {
        if (step > 0) {
            if (end <= start) return 0;
            return (end - start + step - 1) / step;
        }
        if (end >= start) return 0;
        return (start - end + (-step) - 1) / (-step);
    }
    std::string inspect() const override {
        std::string out = "range(" + std::to_string(start) + ", " + std::to_string(end);
        if (step != 1) out += ", " + std::to_string(step);
        return out + ")";
    }
};

// Builtin function object implemented in C++.
// CallFn: lets a builtin invoke Lovax functions (for each/filter/emit) — RFC-005
class BuiltinObject : public Object {
public:
    using CallFn = std::function<Ref<Object>(
        const Ref<Object>& fn,
        const std::vector<Ref<Object>>& args,
        int line)>;

    using BuiltinFn = std::function<Ref<Object>(
        const std::vector<Ref<Object>>&, int line, const CallFn&)>;

    std::string name;
    BuiltinFn fn;

    BuiltinObject(const std::string& n, BuiltinFn f)
        : Object(ObjectType::BUILTIN), name(n), fn(std::move(f)) {}
    std::string inspect() const override { return "builtin " + name + "(...)"; }
};

// Signal object: holds a list of listeners — signal()/connect()/emit() (RFC-005)
class SignalObject : public Object {
public:
    SignalObject() : Object(ObjectType::SIGNAL) {}
    std::vector<Ref<Object>> listeners;
    void gcMark() override { for (auto& l : listeners) gcMarkObject(l.get()); }
    std::string inspect() const override {
        return "signal(" + std::to_string(listeners.size()) + " listeners)";
    }
};

class ReturnValueObject : public Object {
public:
    Ref<Object> value;
    ReturnValueObject(Ref<Object> val)
        : Object(ObjectType::RETURN_VALUE), value(val) {}
    void gcMark() override { gcMarkObject(value.get()); }
    std::string inspect() const override { return value->inspect(); }
};

// break/continue signals: bubble up from the loop body; the loop catches them
class BreakSignalObject : public Object {
public:
    int srcLine;
    BreakSignalObject(int l) : Object(ObjectType::BREAK_SIGNAL), srcLine(l) {}
    std::string inspect() const override { return "break"; }
};

class ContinueSignalObject : public Object {
public:
    int srcLine;
    ContinueSignalObject(int l) : Object(ObjectType::CONTINUE_SIGNAL), srcLine(l) {}
    std::string inspect() const override { return "continue"; }
};

// Runtime error: propagates as a value; the first error stops execution
class ErrorObject : public Object {
public:
    std::string message;
    int srcLine;
    bool compileTime = false;   // set for errors caught while compiling (operand limits)
    // RFC-022: when a STRUCTURED value is thrown (throw {"kind": ..}), the
    // original value rides along and catch binds IT, not the stringified
    // message — e.kind / e["kind"] then work naturally in the handler.
    Ref<Object> payload;
    ErrorObject(const std::string& msg, int l = 0)
        : Object(ObjectType::ERROR), message(msg), srcLine(l) {}
    void gcMark() override { gcMarkObject(payload.get()); }
    std::string inspect() const override {
        const char* tag = compileTime ? "[Compile Error]" : "[Runtime Error]";
        if (srcLine > 0) {
            return std::string(tag) + " line " + std::to_string(srcLine) + ": " + message;
        }
        return std::string(tag) + " " + message;
    }
};

inline bool isError(const Ref<Object>& obj) {
    return obj != nullptr && obj->type() == ObjectType::ERROR;
}

inline Ref<ErrorObject> makeError(const std::string& msg, int line = 0) {
    return makeObj<ErrorObject>(msg, line);
}

// Returns the type name (for the kind() builtin and error messages)
inline std::string typeName(ObjectType t) {
    switch (t) {
        case ObjectType::INTEGER:      return "int";
        case ObjectType::FLOAT:        return "float";
        case ObjectType::BOOLEAN:      return "bool";
        case ObjectType::NULL_OBJ:     return "null";
        case ObjectType::STRING:       return "string";
        case ObjectType::LIST:         return "list";
        case ObjectType::TUPLE:        return "tuple";
        case ObjectType::MAP:          return "map";
        case ObjectType::SET:          return "set";
        case ObjectType::BYTES:        return "bytes";
        case ObjectType::COMPLEX:      return "complex";
        case ObjectType::DEQUE:        return "deque";
        case ObjectType::RANGE:        return "range";
        case ObjectType::FUNCTION:     return "fn";
        case ObjectType::BUILTIN:      return "builtin";
        case ObjectType::SIGNAL:       return "signal";
        case ObjectType::COROUTINE:    return "coroutine";
        case ObjectType::STRUCT:       return "struct";
        case ObjectType::RETURN_VALUE: return "return";
        case ObjectType::BREAK_SIGNAL: return "break";
        case ObjectType::CONTINUE_SIGNAL: return "continue";
        case ObjectType::ERROR:        return "error";
        case ObjectType::BOXED_INT:    return "int";   // transparent to the language
        case ObjectType::NATIVE:       return "native";
    }
    return "?";
}

// ===== Shared semantic helpers =====
// The evaluator AND builtins (contains, find, sort_by, filter) use the same rules.

// Deep equality: numbers compare across types (5 == 5.0), lists/maps by content, functions by identity
inline bool valueEquals(const Value& a, const Value& b);   // defined in value.hpp; lists compare Values
inline bool objectEquals(const Ref<Object>& a, const Ref<Object>& b) {
    bool aNum = (a->type() == ObjectType::INTEGER || a->type() == ObjectType::FLOAT);
    bool bNum = (b->type() == ObjectType::INTEGER || b->type() == ObjectType::FLOAT);
    // complex == complex, and 3+0j == 3 (Python rule)
    if (a->type() == ObjectType::COMPLEX || b->type() == ObjectType::COMPLEX) {
        double ar, ai, br, bi;
        if (a->type() == ObjectType::COMPLEX) {
            auto* ca = static_cast<ComplexObject*>(a.get()); ar = ca->re; ai = ca->im;
        } else if (aNum) {
            ar = (a->type() == ObjectType::INTEGER)
                 ? (double)static_cast<IntegerObject*>(a.get())->value
                 : static_cast<FloatObject*>(a.get())->value;
            ai = 0.0;
        } else return false;
        if (b->type() == ObjectType::COMPLEX) {
            auto* cb = static_cast<ComplexObject*>(b.get()); br = cb->re; bi = cb->im;
        } else if (bNum) {
            br = (b->type() == ObjectType::INTEGER)
                 ? (double)static_cast<IntegerObject*>(b.get())->value
                 : static_cast<FloatObject*>(b.get())->value;
            bi = 0.0;
        } else return false;
        return ar == br && ai == bi;
    }
    if (aNum && bNum) {
        double av = (a->type() == ObjectType::INTEGER)
                    ? (double)static_cast<IntegerObject*>(a.get())->value
                    : static_cast<FloatObject*>(a.get())->value;
        double bv = (b->type() == ObjectType::INTEGER)
                    ? (double)static_cast<IntegerObject*>(b.get())->value
                    : static_cast<FloatObject*>(b.get())->value;
        return av == bv;
    }

    if (a->type() != b->type()) return false;

    switch (a->type()) {
        case ObjectType::NULL_OBJ: return true;
        case ObjectType::BOOLEAN:
            return static_cast<BooleanObject*>(a.get())->value ==
                   static_cast<BooleanObject*>(b.get())->value;
        case ObjectType::STRING:
            return static_cast<StringObject*>(a.get())->value ==
                   static_cast<StringObject*>(b.get())->value;
        case ObjectType::LIST:
        case ObjectType::TUPLE: {
            auto* la = static_cast<ListObject*>(a.get());
            auto* lb = static_cast<ListObject*>(b.get());
            if (la->elements.size() != lb->elements.size()) return false;
            for (size_t i = 0; i < la->elements.size(); ++i) {
                if (!valueEquals(la->elements[i], lb->elements[i])) return false;
            }
            return true;
        }
        case ObjectType::MAP: {
            auto* ma = static_cast<MapObject*>(a.get());
            auto* mb = static_cast<MapObject*>(b.get());
            if (ma->entries.size() != mb->entries.size()) return false;
            for (const auto& e : ma->entries) {
                auto bv = mb->get(e.first);
                if (bv == nullptr || !objectEquals(e.second, bv)) return false;
            }
            return true;
        }
        case ObjectType::BYTES:
            return static_cast<BytesObject*>(a.get())->data ==
                   static_cast<BytesObject*>(b.get())->data;
        case ObjectType::SET: {
            // Order-insensitive: same size and every element present in the other.
            auto* sa = static_cast<SetObject*>(a.get());
            auto* sb = static_cast<SetObject*>(b.get());
            if (sa->entries.size() != sb->entries.size()) return false;
            for (const auto& e : sa->entries) {
                if (sb->get(e.first) == nullptr) return false;
            }
            return true;
        }
        case ObjectType::RANGE: {
            auto* ra = static_cast<RangeObject*>(a.get());
            auto* rb = static_cast<RangeObject*>(b.get());
            return ra->start == rb->start && ra->end == rb->end && ra->step == rb->step;
        }
        case ObjectType::STRUCT: {
            auto* sa = static_cast<StructInstanceObject*>(a.get());
            auto* sb = static_cast<StructInstanceObject*>(b.get());
            if (sa->shape != sb->shape) return false;   // different struct types
            for (size_t i = 0; i < sa->slots.size(); ++i) {
                if (!objectEquals(sa->slots[i], sb->slots[i])) return false;
            }
            return true;
        }
        default:
            return a.get() == b.get(); // functions/signals: identity comparison
    }
}

// Truthiness rules (Python model): null, false, 0, 0.0, "", [], {} -> false
inline bool objectTruthy(const Ref<Object>& obj) {
    switch (obj->type()) {
        case ObjectType::NULL_OBJ: return false;
        case ObjectType::BOOLEAN:  return static_cast<BooleanObject*>(obj.get())->value;
        case ObjectType::INTEGER:  return static_cast<IntegerObject*>(obj.get())->value != 0;
        case ObjectType::FLOAT:    return static_cast<FloatObject*>(obj.get())->value != 0.0;
        case ObjectType::STRING:   return !static_cast<StringObject*>(obj.get())->value.empty();
        case ObjectType::LIST:     return !static_cast<ListObject*>(obj.get())->elements.empty();
        case ObjectType::MAP:      return !static_cast<MapObject*>(obj.get())->entries.empty();
        case ObjectType::RANGE:    return static_cast<RangeObject*>(obj.get())->length() > 0;
        default: return true;
    }
}

// ===== UTF-8 helpers =====
// Turkish characters are multi-byte, so len/indexing counts code points.

inline int utf8CharLen(unsigned char lead) {
    if (lead < 0x80) return 1;
    if ((lead & 0xE0) == 0xC0) return 2;
    if ((lead & 0xF0) == 0xE0) return 3;
    if ((lead & 0xF8) == 0xF0) return 4;
    return 1; // malformed byte: count as one byte to guarantee progress
}

inline long long utf8Length(const std::string& s) {
    long long count = 0;
    size_t i = 0;
    while (i < s.size()) {
        i += utf8CharLen(static_cast<unsigned char>(s[i]));
        count++;
    }
    return count;
}

// Lists the byte offsets of code points (for slice/reverse/find)
inline std::vector<size_t> utf8Offsets(const std::string& s) {
    std::vector<size_t> offs;
    size_t i = 0;
    while (i < s.size()) {
        offs.push_back(i);
        i += utf8CharLen(static_cast<unsigned char>(s[i]));
    }
    offs.push_back(s.size()); // end boundary
    return offs;
}

// Returns the i-th code point as a one-character string ("" = out of range)
inline std::string utf8At(const std::string& s, long long idx) {
    size_t i = 0;
    long long count = 0;
    while (i < s.size()) {
        int len = utf8CharLen(static_cast<unsigned char>(s[i]));
        if (count == idx) return s.substr(i, len);
        i += len;
        count++;
    }
    return "";
}

} // namespace Lovax

#endif // OBJECT_HPP
