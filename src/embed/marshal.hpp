#ifndef LOVAX_EMBED_MARSHAL_HPP
#define LOVAX_EMBED_MARSHAL_HPP

// Marshalling helpers between plain C++ values and the Lovax object model
// (RFC-025). Native functions receive/return Ref<Object>; these turn the
// common host types into objects and read them back with a type check.
// Binary data crosses as `bytes` (BytesObject), the FFI foundation from RFC-018.

#include <string>
#include <cstdint>
#include "../object/object.hpp"

namespace Lovax {
namespace Embed {

// ---- host C++ value  ->  Lovax object ----
inline Ref<Object> box(long long v)        { return makeObj<IntegerObject>(v); }
inline Ref<Object> box(int v)              { return makeObj<IntegerObject>((long long)v); }
inline Ref<Object> box(double v)           { return makeObj<FloatObject>(v); }
inline Ref<Object> box(bool v)             { return boolObj(v); }
inline Ref<Object> box(const std::string& v) { return makeObj<StringObject>(v); }
inline Ref<Object> box(const char* v)      { return makeObj<StringObject>(std::string(v)); }
inline Ref<Object> boxNil()                { return NULL_OBJ_; }
inline Ref<Object> boxBytes(const void* p, size_t n) {
    return makeObj<BytesObject>(std::string(static_cast<const char*>(p), n));
}

// Opaque host handle. `owning`: pass a finalizer to free the C++ object when the
// script drops the handle; `borrowed`: pass nullptr and keep engine ownership.
inline Ref<Object> boxNative(void* ptr, uint32_t typeId,
                             void (*finalize)(void*), const char* typeName) {
    return makeObj<NativeObject>(ptr, typeId, finalize, typeName);
}

// ---- Lovax object  ->  host C++ value (checked; false = wrong type) ----
inline bool asInt(const Ref<Object>& o, long long& out) {
    if (o->type() == ObjectType::INTEGER) { out = static_cast<IntegerObject*>(o.get())->value; return true; }
    if (o->type() == ObjectType::BOXED_INT) { out = static_cast<BoxedIntObject*>(o.get())->value; return true; }
    return false;
}
inline bool asFloat(const Ref<Object>& o, double& out) {
    if (o->type() == ObjectType::FLOAT)   { out = static_cast<FloatObject*>(o.get())->value; return true; }
    if (o->type() == ObjectType::INTEGER) { out = (double)static_cast<IntegerObject*>(o.get())->value; return true; }
    if (o->type() == ObjectType::BOXED_INT){ out = (double)static_cast<BoxedIntObject*>(o.get())->value; return true; }
    return false;
}
inline bool asBool(const Ref<Object>& o, bool& out) {
    if (o->type() != ObjectType::BOOLEAN) return false;
    out = static_cast<BooleanObject*>(o.get())->value; return true;
}
inline bool asString(const Ref<Object>& o, std::string& out) {
    if (o->type() != ObjectType::STRING) return false;
    out = static_cast<StringObject*>(o.get())->value; return true;
}
inline bool asBytes(const Ref<Object>& o, const uint8_t*& data, size_t& len) {
    if (o->type() != ObjectType::BYTES) return false;
    auto& d = static_cast<BytesObject*>(o.get())->data;
    data = reinterpret_cast<const uint8_t*>(d.data()); len = d.size(); return true;
}
// Returns the native pointer iff the handle carries the expected typeId, else null.
inline void* asNative(const Ref<Object>& o, uint32_t expectTypeId) {
    if (o->type() != ObjectType::NATIVE) return nullptr;
    auto* n = static_cast<NativeObject*>(o.get());
    return n->typeId == expectTypeId ? n->ptr : nullptr;
}

} // namespace Embed
} // namespace Lovax

#endif // LOVAX_EMBED_MARSHAL_HPP
