# RFC-025: Embed / FFI bridge (v1.0)

Status: IMPLEMENTED (v1.0.0)

## Motivation

Lovax is the scripting language for a future 2D/2.5D game engine. The engine
(a C++ host) must be able to: drive scripts (call a `on_frame(dt)` each frame),
expose its own native API to scripts (`engine.draw_sprite(...)`), and hand
scripts opaque handles to engine objects (an Entity, a Sprite). This RFC is the
bridge that makes that possible — the language-side of "v1.0". The engine itself
is a separate application built ON this bridge.

Peer study: Lua C API (stack + `lua_pcall`), QuickJS (`JSContext`,
`JS_NewCFunction`, `JS_GetException`), Wren (foreign methods + foreign class
`allocate`/`finalize`), Squirrel (userdata/userpointer), mruby
(`mrb_define_method`), Godot GDExtension (C-ABI function-pointer interface).
Methods studied, not copied.

## Scope decision: registration model, not dynamic C-library FFI

There are two very different things called "FFI":

1. **Registration** — the host registers fixed-signature native functions that
   scripts call. Zero-dependency, safe, sandbox-friendly. This is what a game
   engine needs, and what every non-JIT peer does.
2. **Dynamic C-library FFI** — a script declares & calls arbitrary C library
   symbols at runtime (LuaJIT `ffi.cdef`/`ffi.load`). This needs to synthesize a
   calling-convention thunk per signature: either libffi (a dependency, breaks
   zero-dep) or generated machine code (a mini-JIT). **LuaJIT's famous ffi is
   enabled by its JIT.**

v1.0 ships (1). (2) is deliberately deferred to the **v1.x JIT era**, where the
JIT builds the call thunk — exactly as LuaJIT does. No libffi dependency.

## Design

### Native registration (host → script surface)
- `VM::native(name, BuiltinFn)` installs a native global; `VM::nativeModule(
  name, {fns})` installs a `use <name>`-reachable frozen module. Both reuse the
  existing `BuiltinObject` — the same object a stdlib builtin is. No parallel
  system. `BuiltinFn = Ref<Object>(const vector<Ref<Object>>&, int line,
  const CallFn&)`.
- Host modules live in a registry consulted by `getBuiltinModule` AFTER the
  built-ins, so a host cannot shadow a core module.
- Register before the first `interpret()`: the compiler shares the VM's
  `globalsTable_`, so the name resolves to the registered slot.

### Embedding facade (host drives the VM) — `Lovax::Embed::Host`
- `loadFile/loadSource`, `call(fnName, args)` (host → script, over the existing
  `VM::callFromNative`), `getGlobal/setGlobal`, `lastError()`.
- Error model is **return-value based** — no C++ exception crosses the host
  boundary. (The one internal exception, `CompileError`, is caught inside
  `VM::interpret`.)

### Opaque host handles (userdata) — `NativeObject`
- `void* ptr + uint32_t typeId + void(*finalize)(void*) + const char* typeName`.
- **Owning** (`finalize != null`): the GC calls `finalize(ptr)` when the script
  drops the handle — the engine frees its C++ object. Runs during the sweep:
  **must not allocate a Lovax object or re-enter the VM** (Lua `__gc` rule).
- **Borrowed** (`finalize == null`): the engine owns the lifetime; the handle is
  a non-owning view. A native callback verifies `typeId` before dereferencing.

### GC rooting for host-held values
- The tracing GC (RFC-023) has no refcount. It scans VM stacks/globals +
  permanent/temp roots. A value the host keeps in its OWN C++ state between
  frames is invisible → `Heap::hostRoots` + the `Embed::Persistent` RAII handle
  (C++) / `lovax_protect`/`lovax_unprotect` (C). Values used immediately after a
  call (before any allocation) need no rooting — the standard embedding rule.

### Stable C ABI — `lovax.h`
- Opaque `LovaxVM*` and `LovaxValue` (a GC `Object*` in disguise; every
  primitive is boxed so the handle is uniform). No C++ type crosses.
- `LOVAX_ABI_VERSION` for forward-compatibility. C++ is the primary interface
  (the engine is C++); the C ABI is the ABI-stable secondary layer for Tier-3
  bindings, other-language hosts, and plugins (C++ ABI is fragile; C is the
  stable contract).

### Capability: `--allow-ffi`
- `Perms.ffi` + `--allow-ffi` (mirrors `--allow-run`; `--sandbox` denies it,
  `--allow-all` grants it). Engine-registered natives are TRUSTED (the engine
  chose to expose them) and ungated; a host native that wraps something
  dangerous self-gates via `StdLib::perms().ffi`. The flag is also the reserved
  slot for the future dynamic-FFI path.

## Tier 3, revisited

With the bridge in place, Tier-3 (SQLite, crypto, zlib, HTTP) is a **binding
over the registration API**, distributed via the `pkg` system — never core VM
code. This keeps the zero-dependency core intact, exactly as every peer game
language does (none bundle Tier-3 in core).

## Limitations (deliberate)

- **Single runtime**: one global `Heap` + process-global module caches. v1.0
  assumes one script runtime per process (correct for one engine). Multi-runtime
  isolation is future work.
- Dynamic C-library FFI: v1.x (JIT).

## Verification

`examples/embed/host_demo.cpp` (C++) proves host↔script both directions, an
owning-handle finalizer freeing C++ objects on GC, and a `Persistent` surviving
a forced collection — ASan + UBSan clean, and clean under `GC_STRESS` (collect
every allocation). `examples/embed/host_demo_c.c` proves the same over the C ABI
only. Both are pure C/C++ stdlib — zero dependencies. The 6-mode golden battery,
fuzz, sandbox, unit and bench gates stay green (the bridge adds surface, it does
not touch existing execution paths).

## Credits

Lua/LuaJIT, QuickJS, Wren, Squirrel, mruby, Godot GDExtension — studied for
embedding-API shape and lifetime discipline; all implementations are our own.
