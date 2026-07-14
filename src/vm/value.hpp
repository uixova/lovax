#ifndef VALUE_HPP
#define VALUE_HPP

#include <memory>
#include <string>
#include "../object/object.hpp"

// The VM value type. Numbers, booleans and null live INSIDE the value
// (no heap allocation, no refcounting) — this is what makes tight numeric
// loops fast. Everything else (strings, lists, maps, functions, modules)
// stays a heap Object behind a shared_ptr until the GC arrives.

namespace Lume {

enum class VKind : uint8_t { NIL, BOOL, INT, FLOAT, OBJ };

struct Value {
    VKind kind = VKind::NIL;
    union {
        bool b;
        long long i;
        double d;
    };
    Ref<Object> obj; // engaged only when kind == OBJ

    Value() : kind(VKind::NIL), i(0) {}
    static Value nil()                { Value v; v.kind = VKind::NIL;   v.i = 0; return v; }
    static Value boolean(bool x)      { Value v; v.kind = VKind::BOOL;  v.b = x; return v; }
    static Value integer(long long x) { Value v; v.kind = VKind::INT;   v.i = x; return v; }
    static Value real(double x)       { Value v; v.kind = VKind::FLOAT; v.d = x; return v; }
    static Value object(Ref<Object> o) {
        Value v; v.kind = VKind::OBJ; v.i = 0; v.obj = std::move(o); return v;
    }

    bool isNil()   const { return kind == VKind::NIL; }
    bool isBool()  const { return kind == VKind::BOOL; }
    bool isInt()   const { return kind == VKind::INT; }
    bool isFloat() const { return kind == VKind::FLOAT; }
    bool isObj()   const { return kind == VKind::OBJ; }
    bool isNumber() const { return kind == VKind::INT || kind == VKind::FLOAT; }

    double asDouble() const { return kind == VKind::INT ? (double)i : d; }

    bool isObjType(ObjectType t) const { return kind == VKind::OBJ && obj->type() == t; }
};

// Boxes a Value into the heap Object model (for builtins, containers, slow paths).
inline Ref<Object> toObject(const Value& v) {
    switch (v.kind) {
        case VKind::NIL:   return NULL_OBJ_;
        case VKind::BOOL:  return v.b ? TRUE_OBJ : FALSE_OBJ;
        case VKind::INT:   return makeObj<IntegerObject>(v.i);
        case VKind::FLOAT: return makeObj<FloatObject>(v.d);
        case VKind::OBJ:   return v.obj;
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
    if (v.kind == VKind::OBJ) gcMarkObject(v.obj.get());
}

inline bool valueTruthy(const Value& v) {
    switch (v.kind) {
        case VKind::NIL:   return false;
        case VKind::BOOL:  return v.b;
        case VKind::INT:   return v.i != 0;
        case VKind::FLOAT: return v.d != 0.0;
        case VKind::OBJ:   return objectTruthy(v.obj);
    }
    return true;
}

// Deep equality, fast paths for immediates.
inline bool valueEquals(const Value& a, const Value& b) {
    if (a.isNumber() && b.isNumber()) {
        if (a.kind == VKind::INT && b.kind == VKind::INT) return a.i == b.i;
        return a.asDouble() == b.asDouble();
    }
    if (a.kind != b.kind) {
        if (a.kind == VKind::OBJ || b.kind == VKind::OBJ) {
            return objectEquals(toObject(a), toObject(b));
        }
        return false;
    }
    switch (a.kind) {
        case VKind::NIL:  return true;
        case VKind::BOOL: return a.b == b.b;
        case VKind::OBJ:  return objectEquals(a.obj, b.obj);
        default:          return false;
    }
}

// Display string (matches the tree-era say/inspect output exactly).
inline std::string valueInspect(const Value& v) {
    switch (v.kind) {
        case VKind::NIL:   return "null";
        case VKind::BOOL:  return v.b ? "true" : "false";
        case VKind::INT:   return std::to_string(v.i);
        case VKind::FLOAT: return formatFloat(v.d);
        case VKind::OBJ:   return v.obj->inspect();
    }
    return "";
}

inline std::string valueTypeName(const Value& v) {
    switch (v.kind) {
        case VKind::NIL:   return "null";
        case VKind::BOOL:  return "bool";
        case VKind::INT:   return "int";
        case VKind::FLOAT: return "float";
        case VKind::OBJ:   return typeName(v.obj->tag);
    }
    return "?";
}

} // namespace Lume

#endif // VALUE_HPP
