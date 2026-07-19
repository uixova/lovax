#ifndef VALUE_HPP
#define VALUE_HPP

#include <memory>
#include <string>
#include <cstring>
#include "../object/object.hpp"

// The VM value type. Numbers, booleans and null live INSIDE the value
// (no heap allocation, no refcounting) — this is what makes tight numeric
// loops fast. Everything else (strings, lists, maps, functions, modules)
// stays a heap Object behind a shared_ptr until the GC arrives.

namespace Lovax {

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
    bool isBoxedInt() const { return tbits() == mk(T_BOXINT); }

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

// Boxes a Value into the heap Object model (for builtins, containers, slow paths).
inline Ref<Object> toObject(const Value& v) {
    switch (v.tag()) {
        case VKind::NIL:   return NULL_OBJ_;
        case VKind::BOOL:  return v.asBool() ? TRUE_OBJ : FALSE_OBJ;
        case VKind::INT:   return makeObj<IntegerObject>(v.asInt());
        case VKind::FLOAT: return makeObj<FloatObject>(v.asFloat());
        case VKind::OBJ:   return v.asObj();
    }
    return NULL_OBJ_;
}

// Unwraps a heap Object into a Value (numbers/bools/null become immediates).
inline Value fromObject(const Ref<Object>& o) {
    switch (o->type()) {
        case ObjectType::NULL_OBJ: return Value::nil();
        case ObjectType::BOOLEAN:  return Value::boolean(static_cast<BooleanObject*>(o.get())->value);
        case ObjectType::INTEGER:  return Value::integer(static_cast<IntegerObject*>(o.get())->value);
        case ObjectType::FLOAT:    return Value::real(static_cast<FloatObject*>(o.get())->value);
        default:                   return Value::object(o);
    }
}

// GC: mark the object a Value carries. hasPtr/rawPtr, NOT isObj/asObj — under
// NANBOX isObj() hides a boxed int from the language, but the collector must
// still see it or the box gets swept while referenced.
inline void gcMarkValue(const Value& v) {
    if (v.hasPtr()) gcMarkObject(v.rawPtr());
}

// Write barrier over a Value (same rule: any carried pointer, box included).
inline void gcShadeValue(const Value& v) {
    if (v.hasPtr()) gcShade(v.rawPtr());
}

inline bool valueTruthy(const Value& v) {
    switch (v.tag()) {
        case VKind::NIL:   return false;
        case VKind::BOOL:  return v.asBool();
        case VKind::INT:   return v.asInt() != 0;
        case VKind::FLOAT: return v.asFloat() != 0.0;
        case VKind::OBJ:   return objectTruthy(v.asObj());
    }
    return true;
}

// Deep equality, fast paths for immediates.
inline bool valueEquals(const Value& a, const Value& b) {
    if (a.isNumber() && b.isNumber()) {
        if (a.isInt() && b.isInt()) return a.asInt() == b.asInt();
        return a.asDouble() == b.asDouble();
    }
    if (a.tag() != b.tag()) {
        if (a.isObj() || b.isObj()) {
            return objectEquals(toObject(a), toObject(b));
        }
        return false;
    }
    switch (a.tag()) {
        case VKind::NIL:  return true;
        case VKind::BOOL: return a.asBool() == b.asBool();
        case VKind::OBJ:  return objectEquals(a.asObj(), b.asObj());
        default:          return false;
    }
}

// Display string (matches the tree-era say/inspect output exactly).
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

inline std::string valueTypeName(const Value& v) {
    switch (v.tag()) {
        case VKind::NIL:   return "null";
        case VKind::BOOL:  return "bool";
        case VKind::INT:   return "int";
        case VKind::FLOAT: return "float";
        case VKind::OBJ:   return typeName(v.asObj()->tag);
    }
    return "?";
}

} // namespace Lovax

#endif // VALUE_HPP
