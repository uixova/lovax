# Changelog

## v1.0.0 — embed / FFI bridge: Lovax is now hostable (RFC-025)

The language can be embedded in a C++ host (a future game engine) and expose a
native API to scripts. This is the bridge the engine is built ON; the engine
itself is a separate application. Scope decided with the user: registration
model (fixed-signature natives, zero-dep, what an engine needs), NOT dynamic
C-library FFI — LuaJIT-style `ffi.cdef` needs a JIT to synthesize the call and
lands in the v1.x JIT era.

- **Native registration**: `VM::native(name, fn)` and `VM::nativeModule(name,
  {fns})` expose C++ functions/modules to scripts, reusing `BuiltinObject` —
  a host module is indistinguishable from a stdlib one. Host modules register
  in a table consulted after the built-ins (can't shadow core).
- **C++ facade** `Lovax::Embed::Host`: loadFile/loadSource, `call(fn, args)`
  (host→script), getGlobal/setGlobal, lastError. Return-value error model — no
  C++ exception crosses the boundary.
- **Opaque userdata** `NativeObject`: an engine handle (void* + typeId +
  finalizer + type name). Owning mode frees the C++ object on GC collect;
  borrowed mode leaves lifetime to the engine.
- **Host GC roots**: `Heap::hostRoots` + the `Persistent` RAII handle
  (`lovax_protect`/`unprotect` in C) keep a script value alive across frames —
  the tracing GC has no refcount.
- **Stable C ABI** `lovax.h`: opaque `LovaxVM*`/`LovaxValue`,
  `LOVAX_ABI_VERSION`, full register/eval/call/globals surface. C++ is primary;
  the C ABI is the ABI-stable layer for plugins, Tier-3 bindings, other-language
  hosts (C++ ABI is fragile; C is the stable contract).
- **Capability** `--allow-ffi` (+ `Perms.ffi`): the sandbox slot for untrusted
  native access; engine natives are trusted/ungated, a dangerous native
  self-gates via `perms().ffi`.
- **Tier 3, unblocked**: SQLite/crypto/zlib/HTTP are now bindings over this
  bridge, shipped via `pkg` — never core VM code, zero-dep core preserved.

Demos (`examples/embed/`, zero-dependency): `host_demo.cpp` proves host↔script
both ways + an owning finalizer freeing C++ objects on GC + a Persistent
surviving a forced collect (ASan/UBSan clean AND clean under GC_STRESS);
`host_demo_c.c` proves the same over the C ABI only. New gate `tests/embed.sh`
(in CI) builds/runs both and checks the --allow-ffi gate.

Gates: golden 85/85 unchanged in all six build modes (the bridge adds surface,
touches no execution path); fuzz, sandbox, unit, bench, embed all green.

## v0.18.1 — int64 overflow is now SPEC: two's-complement wrap (hardening)

Pre-engine final audit findings, closed:

- **Crash fixed**: `INT64_MIN / -1` and `INT64_MIN % -1` raised SIGFPE (a
  hardware trap the fuzzer couldn't reach — the literal parse path can't
  produce INT64_MIN directly). Guarded: `/ -1` = wrapped negation, `% -1` = 0.
- **Signed overflow was UB**, now defined: int64 `+ - * <<` and negation wrap
  two's-complement on EVERY platform (unsigned-arithmetic internally), in the
  VM fast paths AND the runtime slow path. Same bits as before on x86 — zero
  golden churn — but now portable, deterministic, and UBSan-clean.
- New golden `59-int-tasma`: INT64_MIN/MAX edges, wrap identities,
  the two trap guards — bit-identical in both value layouts, UBSan-proven.
- CI: 16-byte fallback (`LOVAX_NO_NANBOX`) build+golden step added, as
  RFC-024 promised.

## v0.18.0 — 8-byte NaN-boxed Value with exact int64 (RFC-024)

The 16-byte tagged union became an 8-byte NaN-boxed value — the layout the
JIT (v1.x) will assume, landed and proven BEFORE the FFI ABI freezes.
A value now fits one register.

- **Layout**: doubles as their own bits (NaN canonicalized on write — the
  x86 negative quiet NaN would collide with the tag range); everything else
  top-13-bits-set + 4-bit tag + 47-bit payload. Ints in [-2^46, 2^46) ride
  inline; a rarer int64 is heap-boxed transparently (own tag, so NO type
  query ever dereferences — only an actual boxed read does). **Exact int64
  semantics fully preserved** — the hybrid LuaJIT itself doesn't offer
  (it drops to int32-or-double; we refused).
- **Phase 1 first**: every direct field access (~150 sites) went behind an
  accessor API with the fields made private — the compiler proves the layout
  is swappable. The 16-byte union stays as `LOVAX_NO_NANBOX` (auto on
  non-64-bit targets) and keeps running in CI.
- New golden `58-buyuk-int`: 2^53+1 arithmetic/keys/compares — bit-identical
  in BOTH layouts (the "JS/LuaJIT can't do this" proof).
- One behavior fix: NaN now always prints "nan" (never "-nan" — that was a
  libc accident, at odds with the determinism promise).

**Measured (interleaved best-of-7): fib 232→194 ms (−16%), btree −8%,
everything else equal — no bench regressed.** The full win arrives with the
JIT: type guard = one tag compare, unbox = shift/mask.

Gates: golden 84/84 × 6 modes (NANBOX CG/NOCG/STRESS+ASan/STRESS_INC+ASan +
16B-fallback CG/STRESS+ASan), fuzz 0 crashes, sandbox, unit (Value size
check now layout-aware), bench no-regression.

## v0.17.0 — runtime acceleration, Track A (allocator + map index)

First slice of the pre-JIT runtime work: the speed that must come from data
structures, not from codegen (studying LuaJIT showed its low numbers are
runtime work + JIT together — `lj_alloc.c`, `lj_tab.c` lessons, own designs).

- **Size-class pool allocator.** Every VM object allocation is now one
  free-list pop; a sweep-freed object is one push (16 classes, 16..256 B,
  oversized falls back to `operator new`). Objects carry their size class in
  a spare padding byte. GC_STRESS builds keep plain new/delete so ASan still
  catches a missed root; the pool path itself is validated under ASan with
  redzoned arenas.
- **Map key index rewritten.** One flat power-of-two open-addressing table
  (cached 64-bit hash + entry position per slot) replaces the two
  std::unordered_map typed indexes: no per-node allocation, no key-string
  copy, contiguous probing. Full-bytes FNV-1a + avalanche (a sampled hash
  goes quadratic on adversarial keys — LuaJIT #555). Insertion order, the
  public MapObject API and every golden output are unchanged.

**Measured (interleaved best-of-5, same machine):** gc 46→28 ms (−39%),
hashmap 168→91 ms (−46%), btree 125→109 ms (−13%), heavy_loop 315→156 ms
(−50%); hashmap peak RSS 66→56 MB, everything else flat. fib/strcat
unchanged as designed — fib is the JIT's job (v1.x), strcat is copy-bound
and needs escape analysis (Track B); in-place append without it would be
unsound (aliasing), so it was deliberately not done.

String interning evaluated and deferred: the remaining gain is small next
to the GC-coupled weak-table risk; revisit at v0.18 when the 8-byte value
representation touches StringObject anyway.

Gates: golden 83/83 in five build modes (CG, NOCG, GC_STRESS+ASan,
GC_STRESS_INC+ASan, pool+ASan), fuzz 0 crashes, sandbox, unit, net-multi,
bench no-regression.

## v0.16.0 — incremental GC: the ≤1 ms pause contract (RFC-023)

The stop-the-world collector became a tri-color incremental one: MARK and
SWEEP proceed in 3000-object slices at the existing safepoints; a Dijkstra
write barrier (child-shading; MapObject chokepoint + the bounded VM/builtin
write set) keeps marking sound while the program runs; stacks stay
barrier-free via an atomic root re-scan at finish; sweep-phase births live on
a side list until the cycle ends.

**Measured: btree max pause 13.6 ms → 0.18 ms (74×), gc bench → 0.19 ms;
a particles game frame triggers zero collections.** Memory peaks and run
times unchanged. Known granularity note: one giant map still scans
atomically (hashmap 4.9 ms) — same per-object granularity as Lua.

Verification gained a second stress mode: `LOVAX_GC_STRESS_INC` runs
one-object slices for maximal interleaving — the barrier torture test. Both
stress modes: 81/81 golden bit-for-bit + ASan/UBSan/Leak clean. (That gate
caught a real pre-release miscollection: sweep-born objects allocated black
leaked stale color into the next cycle — fixed structurally, RFC-023.)

## v0.15.0 — robustness: structured errors, testing, quality gates

- **Structured errors (RFC-022).** `throw` carries maps/structs/lists/tuples
  intact — `catch e` binds the ORIGINAL value, so `e.kind` / `e.msg` work in
  the handler. New `error(kind, msg)` builtin builds the convention's map.
  Strings and builtin runtime errors still bind the message string: all
  pre-existing golden outputs stayed byte-identical.
- **testing module.** `assert_eq / assert_true / assert_error` + `summary()`
  (prints the report, returns the fail count — exit-code friendly). Lovax
  code can now test Lovax code.
- **Quality backlog closed** (tracked since v0.10):
  - C++ unit tests (`tests/unit.sh`): value size, cross-type equality, map
    typed indexes, payload-aware gcBytes, civil-date round-trips incl. leap
    days and pre-epoch, the regex compiler's error paths, deterministic RNG
    replay. Zero framework, exit code = failures.
  - Coverage measurement (`tests/coverage.sh`, gcov): **76.1% of src/ lines**
    exercised by the 81-script golden suite — the number is now tracked.
  - Real multi-client network test (`tests/net_multi.sh`): one Lovax TCP
    server serves three separate client processes; both sides asserted.

Gates: 81/81 golden both dispatch modes, GC_STRESS+ASan clean, fuzz +
sandbox + unit + net-multi green, bench flat.

## v0.14.0 — stdlib completion: "fark kalmasın" (the no-gap release)

Every Tier 1+2 row of the Python-3.16/Lua-5.5 gap analysis is now closed or
deliberately deferred — the full accounting lives in docs/feature-matrix.md.
Eight new modules, all zero-dependency, all our own implementations.

- **random** (RFC-020) — uniform/normal/exponential/poisson/gamma/beta/
  triangular + choice/shuffle/sample/choices (weighted) + getstate/setstate.
  All OWN algorithms over two deterministic primitives on the raw mt19937_64
  stream; std::random's distributions vary per platform and would break the
  replay promise. The global random()/seed and game.pick/shuffle/chance now
  ride the same core: `seed(42)` reproduces bit-identically everywhere.
- **datetime** (RFC-019) — now/make/add/diff/format/parse/weekday over our own
  civil-calendar math (no libc tz tables). A datetime is a plain map.
- **regex** (RFC-021) — our own backtracking-VM engine: match/search/find_all/
  replace ($0-$9)/split/groups; greedy+lazy quantifiers, classes, groups,
  anchors. A 1M-step budget turns ReDoS into a clean error, a 10k-instruction
  cap kills expansion bombs. **Benchmark: 3× faster than CPython's re and
  Node's regex** on the cross-harness workload.
- **json** — parse/text in memory (shares the save_data core). Beats CPython
  on the cross-bench (80 vs 101 ms).
- **collections** — counter, setdefault, deque (new DequeObject, O(1) ends).
- **iters** — chain/product/combinations/permutations/accumulate/takewhile/
  dropwhile/groupby/repeat/zip_longest (eager, 1M result cap).
- **functools** — partial, memoize (captures permanent-rooted for the GC).
- **log** — debug/info/warn/error + set_level + to_file (write-gated),
  timestamped.
- **math** grew pi/tau/e/inf/nan, log10/log2, hypot, copysign, is_finite,
  prod; **strings** grew partition (returns a tuple) and fmt(x, ".2f"/"0Nd"/
  "x"); **file** grew glob (own wildcard matcher) and walk — read-gated;
  **os.run(cmd)** is the first consumer of the --allow-run capability
  (sandbox.sh asserts deny and grant).
- Language: keywords are valid member names after '.' (regex.match), unknown
  string escapes pass through literally ("\d+" reads naturally).

Gates: 79/79 golden bit-for-bit both dispatch modes, GC_STRESS+ASan clean,
fuzz + sandbox green (now incl. --allow-run), bench flat, 1273-call module
type-fuzz under ASan: 0 crashes. Cross-bench regex+json rows added.

## v0.13.0 — core types: tuple, Set, bytes, complex (RFC-018)

Four missing fundamentals from the Python/Lua gap analysis, each a new GC
object type — no value-representation change, no perf regression.

- **tuple** — `(a, b)`, one-tuple `(5,)`, empty `()`; **multi-return**
  `return a, b` + unpacking `set q, r = f()`; indexing, iteration, `in`,
  deep equality; immutable (shares the list layout, the tag blocks mutation).
- **Set** — `Set([1, 2, 3])` (capital S: lowercase `set` is the keyword);
  `a | b` / `a & b` / `a - b`, `in`, order-insensitive `==`, `add/remove/has/
  values/clone`; insertion-ordered, prints `Set{1, 2}`. A value-less map
  underneath — indexes and iteration reused.
- **bytes** — `bytes([72, 101])` / `bytes("text")`; index → int, slice, `len`,
  `text()` decodes; prints `b"He\x01"`. **Breaking:** `file.read_bytes` now
  returns bytes (was list-of-ints); `write_bytes` accepts both; `net.send`
  accepts bytes. Foundation for future hash/compress modules.
- **complex** — `3 + 4j` literal; `+ - * / **`, unary `-`, `.real/.imag`,
  `abs(z)`, `3+0j == 3`; `use cmath` → `make, sqrt, exp, log, sin, cos, conj,
  abs, arg, polar`.

Gates: 74/74 golden bit-for-bit both dispatch modes, GC_STRESS+ASan clean,
fuzz/sandbox green, bench flat, 3150-call new-type fuzz under ASan: 0 crashes.

## v0.12.0 — modular layout + the memory release

The cross-language benchmark (benchmarks/cross) exposed struct memory as the
worst number in the language: **btree peaked at 270 MB vs Lua's 24 MB**. v0.12
fixes the two root causes and reorganizes the stdlib into Lua-style one-file
modules. Result, same machine: **btree 270 → 37 MB and 653 → 216 ms; gc bench
30 → 15 MB — Lovax now sits in the Lua tier on every memory benchmark.**

### One file per module (`src/modules/`)
- `builtins.hpp` (1022 lines) + `stdlib.hpp` (2191) split into `src/modules/`:
  `common` (shared helpers, permission gates, JSON core), `base` (global
  builtins), `math`, `game`, `strings`, `file`, `os`, `time`, `canvas`, `net`.
  The old headers remain as thin include points — nothing upstream changed.
  Adding a future module (json, regex, datetime…) is now a one-file affair.

### Compact struct instances (RFC-017)
- A struct instance was a full `MapObject`: entries for `__type__`, every field
  **and every method closure**, plus three per-instance hash indexes. Now one
  `StructShapeObject` per type (field→slot map + method table, built once at
  declaration) and instances are a shape pointer + flat slot array. The member
  inline-cache stores the slot index, verified against the shape.
- **Breaking:** writing an *undeclared* field (`p.z = 1`) is now a runtime
  error — `struct 'P' has no field 'z' (fields are fixed at declaration)`.
  This catches field typos at the assignment site and is what makes the flat
  layout possible. `p["field"]`, `keys/values/len/has/get/copy/clone`,
  equality and printing all still work on instances.

### Honest GC accounting + `--mem-stats`
- `bytesAllocated` counted headers only and was **never reduced at sweep**, so
  the "collect when heap doubles" threshold doubled off a forever-growing
  total — collections got rarer as programs ran and garbage piled up. The
  sweep now recomputes **live** bytes (payload-aware: string capacity, vector
  storage, map indexes, coroutine stacks); `nextGC = live × 1.5` (4 MB floor).
- `lovax --mem-stats script.lov` prints allocations / collections / peak /
  total GC time / max pause — the measurement rig for the coming incremental
  GC's ≤1 ms pause budget.

### Gates
- 70/70 golden bit-for-bit in both dispatch modes (2 new struct cases),
  GC_STRESS + ASan/UBSan/Leak clean, fuzz (71 inputs) and sandbox green,
  no time regressions (`tests/bench.sh` and the cross harness).

## v0.11.0 — value representation: a tracing garbage collector

The value model is rebuilt for the engine era (RFC-013). `std::shared_ptr<Object>`
is retired in favor of a **tracing mark-sweep garbage collector** and a **16-byte
tagged value** — the prerequisites for the JIT and the game engine's memory
budget. Same language, 67 golden tests bit-for-bit in both dispatch modes.

### Tracing GC (RFC-013)
- `src/vm/gc.hpp`: an intrusive mark-sweep heap. Allocation never collects
  mid-operation; it flags a request the VM services at **safepoints** (loop
  back-edges + calls), where the value stack is the complete root set. This is
  what makes a precise collector safe to retrofit onto a VM that holds objects in
  C++ locals throughout.
- `Ref<T>`: a one-word non-owning pointer that preserved the `shared_ptr` call
  sites, so the conversion was near-churn-free. Precise roots across every live
  VM (main + modules), coroutine states, singletons, and permanent-rooted
  builtin modules; per-type `gcMark` walks the object graph.

### 16-byte value (not NaN-box)
- `Value` is now 16 bytes (was 32): tag + `union{int64, double, Object*}`. This
  matches **Lua 5.4**, the int64 peer. An 8-byte NaN-box was rejected: its ~48-bit
  payload cannot hold Lovax's native int64 (LuaJIT NaN-boxes only because it lacks
  int64), and it carries per-platform pointer-bit hazards. Portable and exact.

### Safety (the whole point)
- `LOVAX_GC_STRESS` collects on **every** allocation, turning any missed root into
  an immediate use-after-free. Under GC-stress + ASan/UBSan, all 67 golden + the
  fuzz and sandbox gates are **clean**. Heavy allocation stays memory-bounded.

### Post-release hardening (pre-engine audit, 2026-07-17)
- **Compile-time operand-overflow guards (correctness fix).** An audit found a
  silent miscompile: fixed-width bytecode operands (constant index / jump distance
  / global & local slot u16, argument count u8) were written from counts and
  offsets with no bound check, so a program past a limit truncated the operand and
  ran wrong code with no error (proven: >65535 constants loaded the wrong one).
  Every truncation site now raises a located `[Compile Error]` (exit 70) instead.
  New golden `e22` locks the argument-count limit.
- **Rename completion.** The Lume→Lovax rename had left CI (built `lume`, stale
  `LUME_*` macro, `*.lm` globs → CI fully broken), the release workflow (`lume-*`
  asset names), `.gitignore` (tracked the `lovax` binary), and the examples'
  run instructions inconsistent. All fixed; CI hardened (ASan sweep now fails on
  any sanitizer report; a GC_STRESS root-completeness job was added).
- **Honest benchmarks.** A cross-language harness (`benchmarks/cross/`, Lovax vs
  Lua 5.4/5.5, LuaJIT, CPython, Node; outputs verified identical) replaces the
  optimistic single-language claims. Current reality: Lovax trails on compute
  (~2× Lua, ~1.3× CPython, ~10× LuaJIT), wins startup, and is memory-heavy
  (binary-tree 270 MB vs Lua 24 MB — struct instances are map-backed). README and
  ROADMAP corrected to match. Next levers: compact struct layout, incremental GC.
- **Predictive audit result:** 3648 wrong-type/arity builtin calls, GC_STRESS +
  ASan over all golden, fuzz, and sandbox — **0 crashes**. Type-guard discipline
  is complete; the 16-byte value avoids NaN-box platform hazards.

## v0.10.0 — general-purpose + safe: net, packages, sandbox

Lovax stops being game-only. It can now open sockets, install version-pinned
packages, and run untrusted code behind a capability sandbox — the pieces a
real general-purpose / server / scripting language needs. (The value-model
perf work — NaN-boxing, GC — is scoped separately as v0.11, RFC-013, so it
gets its own verification cycle rather than being rushed.)

### Networking — `net` (RFC-015)
- Blocking TCP/UDP sockets on OS syscalls (zero third-party deps): tcp_listen/
  accept/connect, send/recv/close, udp_socket/bind/send/recv, set_timeout.
- Enough for a VPS service, LAN multiplayer messaging, or a small client. A
  socket is an int handle; misuse returns a catchable error, never a crash.

### Version-pinned packages (RFC-007 phase 2)
- `lovax install user/repo@v1.2.0` clones exactly that git tag and locks the
  commit SHA in lovax.lock; deps recorded in lovax.json; no-arg install
  reproduces from the manifest. The supply-chain guarantee npm/pip don't give.
- Fixed a pre-existing bug: file-module functions now resolve their module-level
  globals correctly (state persists across calls) even when called from another
  VM — packages with internal state finally work.

### Capability sandbox (RFC-015)
- `lovax --sandbox --allow-net app.lov`: dangerous ops (net, file read/write,
  env) require explicit permission (Deno's model). Mentioning any permission
  flag opts into deny-all; no flag = your own trusted script, all allowed.
- An installed package physically cannot touch the network or filesystem unless
  you allowed it — the runtime half of the malicious-code defense.

### Security gates (permanent, automated)
- `tests/fuzz.sh`: ~70 adversarial/random inputs, never crashes (signal exit is
  a hard fail), run under the ASan build too.
- `tests/sandbox.sh`: asserts every capability denies when sandboxed and allows
  when granted.
- New adversarial golden cases (e18–e21). CI runs golden (both dispatch modes) +
  sanitizers + fuzz + sandbox on every push.

### Quality
- 67 golden tests pass in both dispatch modes; fuzz + sandbox gates green;
  net/package/module/gated paths clean under ASan/UBSan/LeakSanitizer.

## v0.9.0 — coroutines, string speed, distribution

Beats CPython everywhere and Lua 5.4 on string/data work; coroutines land for
game scripting; and Lovax now installs like any other language. Same-machine
best-of-5 vs CPython 3.14 / Lua 5.4:

| Benchmark | Lovax | CPython | Lua 5.4 |
|-----------|-----:|--------:|--------:|
| string suite (concat/interp/keys/eq) | **19 ms** | 240 ms | — |
| member access 1M | **68 ms** | 108 ms | — |
| `fib(30)` (pure recursion) | 66 ms | 79 ms | 44 ms |

### Coroutines (RFC-014)
- `yield` keyword + `spawn` / `resume` / `co_status` / `co_done`. Two-way values,
  nested coroutines, closures across yields, try/finally around yield, error
  propagation to `resume`. Each coroutine owns a saved execution context; the
  VM swaps contexts and re-enters `run()`.

### String package (RFC's typed-map work)
- Typed map indexes: string keys hash raw bytes; int/bool keys never build a
  string. No per-lookup key-tag allocation.
- `ADD_INPLACE`: `t += e` / `t = t + e` append in place for a uniquely-held
  string (CPython's refcount trick); single-reserve concat otherwise.
- UTF-8 length cache. Result: string suite **240 ms → 19 ms**.

### Inline cache
- Per-site member-access cache; `player.hp`-style access hits a remembered slot,
  verified against the live key. member 1M **108 → 68 ms** (past CPython).

### Distribution (v0.9 "download it like any language")
- `curl … install.sh | sh` / `install.ps1`, `lovax update` self-update.
- GitHub Actions: release pipeline (Linux/macOS x64+arm64/Windows binaries +
  checksums) and CI (golden suite in both dispatch modes + sanitizer sweep).

### Deferred (documented, RFC-013)
- NaN-boxing / 8-byte values — the one lever left for `fib` vs Lua. It rewrites
  the whole value model, so it's scheduled as the v0.10 headline with its own
  verification cycle rather than rushed in here.

### Quality
- 62 golden tests (new: 41 string-inplace, 42 coroutines) pass in both dispatch
  modes; ASan/UBSan/LeakSanitizer clean.

## v0.8.0 — the speed release: faster than CPython

The VM was rebuilt for speed ([RFC-012](rfcs/012-vm-performance.md)). Same
language, same error messages, 60 golden tests bit-for-bit — just fast:

| Benchmark | v0.6 | **v0.8** | CPython 3.14 |
|-----------|-----:|---------:|-------------:|
| `fib(30)` (2.7M calls) | 357 ms | **115 ms** | 139 ms |
| `heavy_loop` | 727 ms | **371 ms** | 449 ms |

- **Direct-threaded dispatch** (computed goto) on GCC/Clang, with a tested
  portable switch fallback (`-DLOVAX_NO_COMPUTED_GOTO`).
- **Fused superinstructions** from a compiler peephole: immediate arithmetic
  (`n - 1`, `% 13`, `& 255`), compare-and-branch (`if n < 2` is one op),
  two-local loads, local±immediate.
- **In-place stack arithmetic** — zero Value moves on numeric paths.
- **Call fast path**: exact-arity closure calls inlined; `RETURN` moves the
  result in place; frames cache chunk/const pointers; range `for` loops have an
  all-scalar path.
- Fixed `push()` taking its argument by value — the single biggest win.
- Recommended build: `g++ -std=c++17 -O3 -fno-gcse -fno-crossjumping`.
- Verified: ASan + UBSan + LeakSanitizer clean in both dispatch modes.

## v0.7.1 — per-iteration loop capture + try/finally

- **Closures in `for` loops now capture the loop variable per iteration**
  (`0,1,2`, like C#/Lua/GDScript/JS-`let`) instead of Python's late binding
  (`2,2,2`). Game-critical: `for enemy in enemies: connect(enemy.died, fn() ->
  drop_loot(enemy))` now binds each handler to *its* enemy. Outer variables
  captured in the loop stay shared; the loop variable still leaks after the
  loop. Rationale and survey: [RFC-011](rfcs/011-loop-capture.md).
- Loop variables are real local slots in every scope, including the top-level
  script frame — script-level and function-level loops behave identically, and
  a top-level closure can now capture a top-level loop variable.
- `try:` may be followed by `finally:` alone (no `catch`); the finally block
  runs on the throw path before the error re-raises. Fixed a compiler crash
  (null catch-name deref) and missing local collection inside `finally`.
- New stress suite `examples/stress/` (roguelike, BFS pathfinding, particles,
  economy, edge cases, heavy_loop) + `tests/bench.sh` timing harness — the
  v0.8 performance baseline. 60 golden tests, ASan/UBSan clean.

## v0.7.0 — rich core

The core stops being minimal. New language constructs, a much bigger standard
library, and an interactive REPL — no fake keywords, everything maps to real
bytecode (see [RFC-009](rfcs/009-rich-core.md) for the accept/reject rationale).

### Language
- **`struct` + `this`** — tagged data with methods and default fields;
  `player.hp` is a real field, `this` is implicit in methods. Composition, no
  inheritance ([RFC-010](rfcs/010-struct-enum.md)).
- **`enum`** — named integer constants (`enum State: IDLE, WALK, ATTACK`), inline
  or block form.
- **`try` / `catch` / `throw` / `finally`** — structured, recoverable error
  handling on a VM handler stack; uncaught errors are unchanged ([RFC-008](rfcs/008-error-handling.md)).
- **`?.` null-safe access** — `player?.weapon?.damage` yields null instead of
  crashing on a null link.
- **`->` arrow lambdas** — `fn(x) -> x * 2`.
- **New compound assignments** — `&= |= ^= <<= >>= ??=` (bitwise, shift,
  null-coalescing), joining the existing `+= -= *= /= %=`.
- Plus the earlier v0.7 syntax pack: `const`, `pass`, `repeat N:`, `until cond:`,
  `x is "type"`, `..` ranges, slices `xs[a:b]`, parallel/unpack `set a, b = …`,
  variadic `fn f(rest...)`, `##` block comments.

### Standard library & builtins
- ~20 new core builtins: `enumerate`, `zip`, `all`, `any`, `sorted`, `reduce`,
  `first`, `last`, `flat`, `unique`, `group_by`, `min_by`, `max_by`,
  `binary_search`, `get`, `entries`, `to_map`, `dump`, `exit`, …
- Modules grown: `math` (+`gcd`/`lcm`/`smoothstep`/`inverse_lerp`/`factorial`/…),
  `strings` (+`lines`/`capitalize`/`is_digit`/…), `game` (+`chance`/`noise`),
  `file`, `os`, plus new **`time`** and a terminal **`canvas`** renderer.

### Tooling
- **REPL** — run `lovax` with no arguments for an interactive session; a bare
  expression is echoed, a header line ending in `:` opens a multi-line block.

### Quality
- 58 golden tests pass bit-for-bit (53 prior + 5 new: struct, enum, try/finally,
  operators, null-safe/lambda). Clean under ASan + UBSan.
- Benchmark note: `fib(30)` ≈ 354 ms vs CPython ≈ 133 ms — call-heavy code is
  still slower than CPython. Closing that gap (computed-goto dispatch,
  NaN-boxing, inline caches) is the v0.8 objective, not this release's scope.

## v0.6.0 — bytecode VM

The tree-walking interpreter is retired. Lovax now compiles to bytecode and runs on
a stack-based VM:

- Immediate numeric values (ints/floats/bools/null never touch the heap).
- Function-scoped local slots, closed upvalues for closures, own call frames.
- Fast paths for arithmetic/comparisons and list indexing; object-typed operations
  reuse the exact tree-era semantics, so every error message stays identical.
- All 53 golden tests pass bit-for-bit against the previous release.
- Speed vs the tree-walker: fib 6.4x, mandelbrot 8x, game-loop 3.8x.
  Mandelbrot now beats CPython 3.14; the game-loop matches it. Call-heavy paths
  (fib) close the remaining gap in VM phase 2 (register instructions, NaN-boxing).

## v0.5.0 — first public release

### Language
- Indentation-based, Python-simple syntax; full UTF-8 identifiers (Turkish-friendly).
- Number literals: hex (`0xFF`), binary (`0b1010`), `_` separators, scientific (`1e6`).
- Operators: full arithmetic with floor `/` and `%` (identity-consistent), `**` (right-assoc),
  comparisons with deep equality, `and/or/not` (short-circuit), `in` membership,
  bitwise `& | ^ ~ << >>` (Python precedence), compound assignment `+= -= *= /= %=`.
- Control flow: `if / else if / else`, `match` (multi-pattern, `_` wildcard, no fallthrough),
  ternary (`a if cond else b`), `while`, `for/in` over list/range/string/map, `break/continue`.
- Functions: closures, recursion, default parameters (call-time evaluation),
  anonymous functions, strict arity, call-depth protection.
- Strings: interpolation `"hp: {hp * 2}"` (full expressions), escapes, multiline `"""..."""`.
- Data: list, insertion-ordered map with dot access (`player.hp`), lazy range.
- `set` defines / bare assignment updates (typo protection, RFC-001).

### Module System (RFC-006)
- `use math` / `use x as y` / `use x: a, b` / `use "file.lov"` — built-ins load only when invited.
- Modules are frozen maps; user `.lov` files are modules too (cached, cycle-detected,
  importer-relative paths).

### Built-in Modules
- `math`: lerp, clamp, remap, wrap, move_toward, dist, snap, full trig, exp/log, PI/TAU.
- `game`: Penner easing, pick/pick_weighted/shuffle (loot), signals, poll-based timers.
- `strings`: split, join, Turkish-aware upper/lower, trim, replace, count, padding, chr/ord, fixed.
- `file`: text/lines/binary/JSON/CSV read-write, exists, list_dir, make_dir, delete_file.
- `os`: env, set_env, platform, cwd, args, path_join.

### Packages (RFC-007)
- `lovax install user/repo` fetches libraries into `lovax_libs/`; `use <name>` imports
  installed packages exactly like built-ins.

### Quality
- Located, helpful English error messages; syntax errors prevent execution entirely.
- 53 golden-file tests, benchmark suite, ASan/UBSan clean.
