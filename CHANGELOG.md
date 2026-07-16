# Changelog

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

### Known follow-ups (v0.11.x)
- Incremental GC (≤1 ms pauses, write barriers) — deliberately its own gated
  effort: a missed write barrier reintroduces use-after-free, so it is not rushed.
  The current stop-the-world collector is correct and sufficient pre-engine.
- In-place string append is temporarily off (no cheap uniqueness check under a
  tracing GC); the string suite is still ~5 ms.

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
