// C ABI implementation over the C++ embed facade (RFC-025). Compile this ONE
// translation unit with a C++17 compiler; link it with any C (or other-language)
// host that includes lovax.h. A LovaxValue is just a GC-managed Object* in
// disguise — every primitive is boxed so the handle is uniform and opaque.

#include "embed.hpp"
#include "lovax.h"

using namespace Lovax;
using namespace Lovax::Embed;

struct LovaxVM {
    Host host;
    std::string errText;
};

namespace {
inline Object*    asObj(LovaxValue v) { return reinterpret_cast<Object*>(v); }
inline LovaxValue asVal(const Ref<Object>& r) { return reinterpret_cast<LovaxValue>(r.get()); }
inline LovaxValue asVal(Object* p)            { return reinterpret_cast<LovaxValue>(p); }
}

extern "C" {

int lovax_abi_version(void) { return LOVAX_ABI_VERSION; }

LovaxVM* lovax_new(void) { return new LovaxVM(); }
void     lovax_free(LovaxVM* vm) { delete vm; }

int lovax_eval(LovaxVM* vm, const char* source) {
    return vm->host.loadSource(source ? source : "") ? 0 : 1;
}
int lovax_eval_file(LovaxVM* vm, const char* path) {
    return vm->host.loadFile(path ? path : "") ? 0 : 1;
}

int lovax_call(LovaxVM* vm, const char* fn_name,
               int argc, const LovaxValue* argv, LovaxValue* out) {
    std::vector<Ref<Object>> args;
    args.reserve(argc > 0 ? argc : 0);
    for (int i = 0; i < argc; ++i) args.push_back(Ref<Object>(asObj(argv[i])));
    auto r = vm->host.call(fn_name ? fn_name : "", args);
    if (!r) return 1;
    if (out) *out = asVal(r);
    return 0;
}

void lovax_register(LovaxVM* vm, const char* name, LovaxNativeFn fn) {
    LovaxVM* self = vm;
    std::string nm = name ? name : "";
    vm->host.native(nm, [self, fn](const std::vector<Ref<Object>>& a, int line,
                                   const BuiltinObject::CallFn&) -> Ref<Object> {
        std::vector<LovaxValue> argv;
        argv.reserve(a.size());
        for (auto& x : a) argv.push_back(asVal(x));
        LovaxValue r = fn(self, (int)argv.size(), argv.data());
        return Ref<Object>(asObj(r));
    });
}

int lovax_get_global(LovaxVM* vm, const char* name, LovaxValue* out) {
    auto r = vm->host.getGlobal(name ? name : "");
    if (!r) return 1;
    if (out) *out = asVal(r);
    return 0;
}
void lovax_set_global(LovaxVM* vm, const char* name, LovaxValue v) {
    vm->host.setGlobal(name ? name : "", Ref<Object>(asObj(v)));
}

LovaxValue lovax_nil(void)                        { return asVal(NULL_OBJ_); }
LovaxValue lovax_int(LovaxVM*, long long v)       { return asVal(Embed::box(v)); }
LovaxValue lovax_float(LovaxVM*, double v)        { return asVal(Embed::box(v)); }
LovaxValue lovax_bool(int v)                      { return asVal(boolObj(v != 0)); }
LovaxValue lovax_string(LovaxVM*, const char* s)  { return asVal(Embed::box(std::string(s ? s : ""))); }
LovaxValue lovax_bytes(LovaxVM*, const void* d, size_t n) { return asVal(Embed::boxBytes(d, n)); }
LovaxValue lovax_native(LovaxVM*, void* ptr, uint32_t tid,
                        void (*fin)(void*), const char* tn) {
    return asVal(Embed::boxNative(ptr, tid, fin, tn));
}

int lovax_is_int(LovaxValue v)    { auto* o = asObj(v); return o && (o->type()==ObjectType::INTEGER || o->type()==ObjectType::BOXED_INT); }
int lovax_is_float(LovaxValue v)  { auto* o = asObj(v); return o && o->type()==ObjectType::FLOAT; }
int lovax_is_string(LovaxValue v) { auto* o = asObj(v); return o && o->type()==ObjectType::STRING; }
int lovax_is_bytes(LovaxValue v)  { auto* o = asObj(v); return o && o->type()==ObjectType::BYTES; }

int lovax_as_int(LovaxValue v, long long* out) {
    long long t; if (!Embed::asInt(Ref<Object>(asObj(v)), t)) return 0;
    if (out) *out = t; return 1;
}
int lovax_as_float(LovaxValue v, double* out) {
    double t; if (!Embed::asFloat(Ref<Object>(asObj(v)), t)) return 0;
    if (out) *out = t; return 1;
}
int lovax_as_bool(LovaxValue v, int* out) {
    bool t; if (!Embed::asBool(Ref<Object>(asObj(v)), t)) return 0;
    if (out) *out = t ? 1 : 0; return 1;
}
const char* lovax_as_string(LovaxValue v) {
    auto* o = asObj(v);
    if (!o || o->type() != ObjectType::STRING) return nullptr;
    return static_cast<StringObject*>(o)->value.c_str();
}
const void* lovax_as_bytes(LovaxValue v, size_t* len) {
    auto* o = asObj(v);
    if (!o || o->type() != ObjectType::BYTES) { if (len) *len = 0; return nullptr; }
    auto& d = static_cast<BytesObject*>(o)->data;
    if (len) *len = d.size();
    return d.data();
}
void* lovax_as_native(LovaxValue v, uint32_t tid) {
    return Embed::asNative(Ref<Object>(asObj(v)), tid);
}

void lovax_protect(LovaxValue v) {
    auto* o = asObj(v); if (o) Heap::get().hostRoots.push_back(o);
}
void lovax_unprotect(LovaxValue v) {
    auto* o = asObj(v); if (!o) return;
    auto& hr = Heap::get().hostRoots;
    for (auto it = hr.begin(); it != hr.end(); ++it) {
        if (*it == o) { hr.erase(it); return; }   // remove one instance
    }
}

const char* lovax_last_error(LovaxVM* vm) {
    auto e = vm->host.lastError();
    vm->errText = e ? e->inspect() : "";
    return vm->errText.c_str();
}

} // extern "C"
