/* lovax.h — stable C ABI for embedding Lovax (RFC-025).
 *
 * The C++ facade (embed.hpp) is the primary interface when the host is C++.
 * This C header is the SECONDARY, ABI-stable boundary: a C compiler (or any
 * language with a C FFI) can drive the VM through opaque handles, and native
 * plugins / Tier-3 bindings compile against a versioned contract that the
 * fragile C++ ABI cannot offer. No C++ type crosses this boundary.
 *
 * Values are opaque LovaxValue handles owned by the runtime's GC. A handle you
 * intend to keep across a call that can allocate MUST be protected
 * (lovax_protect) and later released (lovax_unprotect) — the tracing GC has no
 * refcount and will otherwise reclaim it.
 *
 * Header-only usage: define LOVAX_IMPL in exactly one .cpp that also has the
 * Lovax C++ headers on its include path, to emit the implementation.
 */
#ifndef LOVAX_H
#define LOVAX_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LOVAX_ABI_VERSION 1

typedef struct LovaxVM LovaxVM;
typedef uint64_t LovaxValue;            /* opaque; interpret only via the API */

/* A native function exposed to scripts. argv/argc are borrowed for the call;
 * return a LovaxValue (use lovax_make_* / lovax_nil). */
typedef LovaxValue (*LovaxNativeFn)(LovaxVM* vm, int argc, const LovaxValue* argv);

/* ---- lifecycle ---- */
int        lovax_abi_version(void);     /* == LOVAX_ABI_VERSION of the built lib */
LovaxVM*   lovax_new(void);
void       lovax_free(LovaxVM* vm);

/* ---- load + run (returns 0 on success, non-zero on error; see last_error) ---- */
int        lovax_eval(LovaxVM* vm, const char* source);
int        lovax_eval_file(LovaxVM* vm, const char* path);

/* ---- host -> script ---- */
/* Call a global script function by name with argc args. Writes the result to
 * *out (if out != NULL) and returns 0; on error returns non-zero. */
int        lovax_call(LovaxVM* vm, const char* fn_name,
                      int argc, const LovaxValue* argv, LovaxValue* out);

/* ---- native registration (before eval) ---- */
void       lovax_register(LovaxVM* vm, const char* name, LovaxNativeFn fn);

/* ---- globals ---- */
int        lovax_get_global(LovaxVM* vm, const char* name, LovaxValue* out);
void       lovax_set_global(LovaxVM* vm, const char* name, LovaxValue v);

/* ---- value constructors ---- */
LovaxValue lovax_nil(void);
LovaxValue lovax_int(LovaxVM* vm, long long v);
LovaxValue lovax_float(LovaxVM* vm, double v);
LovaxValue lovax_bool(int v);
LovaxValue lovax_string(LovaxVM* vm, const char* s);
LovaxValue lovax_bytes(LovaxVM* vm, const void* data, size_t len);
/* Opaque host handle. finalize (may be NULL = borrowed) runs on GC collect. */
LovaxValue lovax_native(LovaxVM* vm, void* ptr, uint32_t type_id,
                        void (*finalize)(void*), const char* type_name);

/* ---- value accessors (return 0/false on type mismatch) ---- */
int        lovax_is_int(LovaxValue v);
int        lovax_is_float(LovaxValue v);
int        lovax_is_string(LovaxValue v);
int        lovax_is_bytes(LovaxValue v);
int        lovax_as_int(LovaxValue v, long long* out);
int        lovax_as_float(LovaxValue v, double* out);
int        lovax_as_bool(LovaxValue v, int* out);
const char* lovax_as_string(LovaxValue v);      /* NUL-terminated, borrowed */
const void* lovax_as_bytes(LovaxValue v, size_t* len);
void*      lovax_as_native(LovaxValue v, uint32_t expect_type_id);

/* ---- GC rooting for host-held values (no refcount under a tracing GC) ---- */
void       lovax_protect(LovaxValue v);
void       lovax_unprotect(LovaxValue v);

/* Text of the last error on this VM (borrowed, "" if none). */
const char* lovax_last_error(LovaxVM* vm);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LOVAX_H */
