#ifndef VALUE_HPP
#define VALUE_HPP

#include <memory>
#include <string>
#include "../object/object.hpp"

// The VM value type. Numbers, booleans and null live INSIDE the value
// (no heap allocation, no refcounting) — this is what makes tight numeric
// loops fast. Everything else (strings, lists, maps, functions, modules)
// stays a heap Object behind a shared_ptr until the GC arrives.

namespace Lovax {

enum class VKind : uint8_t { NIL, BOOL, INT, FLOAT, OBJ };

// 16-byte tagged value (matches Lua 5.4, which also keeps a native int64). A
// NaN-boxed 8-byte value is a LuaJIT trick that only works WITHOUT a 64-bit int
// (the NaN payload is ~48 bits); Lovax keeps exact int64, so a clean tagged union
// is both correct and portable — no per-platform pointer-bit assumptions.
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
};

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

// Truthiness (Python model), fast path for immediates.
// GC: mark the object a Value points at (immediates carry no pointer).
inline void gcMarkValue(const Value& v) {
    if (v.isObj()) gcMarkObject(v.asObj());
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
