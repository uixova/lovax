#ifndef VALUE_HPP
#define VALUE_HPP

#include <memory>
#include <string>
#include <cstring>
#include "../object/object.hpp"

// The VM value type (`struct Value`) now lives in object.hpp — it was hoisted
// ahead of the container classes so ListObject/TupleObject can store their
// elements UNBOXED (std::vector<Value>): an int/float/bool element rides in the
// 8-byte word instead of a per-element heap object (RFC-024 + unboxed lists).
// Also in object.hpp, next to Value: VKind, gcMarkValue/gcShadeValue,
// valueInspect/valueInspectQuoted (the container methods need them).
//
// This header keeps the value<->Object bridge helpers that need the LATER-
// defined object machinery (objectEquals/objectTruthy/typeName): toObject,
// fromObject, valueTruthy, valueEquals, valueTypeName.

namespace Lovax {

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

// Deep equality, fast paths for immediates. (Forward-declared in object.hpp so
// objectEquals can compare unboxed list elements through it.)
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
