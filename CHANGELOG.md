# Changelog

## v0.8.0 — the speed release: faster than CPython

The VM was rebuilt for speed ([RFC-012](rfcs/012-vm-performance.md)). Same
language, same error messages, 60 golden tests bit-for-bit — just fast:

| Benchmark | v0.6 | **v0.8** | CPython 3.14 |
|-----------|-----:|---------:|-------------:|
| `fib(30)` (2.7M calls) | 357 ms | **115 ms** | 139 ms |
| `heavy_loop` | 727 ms | **371 ms** | 449 ms |

- **Direct-threaded dispatch** (computed goto) on GCC/Clang, with a tested
  portable switch fallback (`-DLUME_NO_COMPUTED_GOTO`).
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
- **REPL** — run `lume` with no arguments for an interactive session; a bare
  expression is echoed, a header line ending in `:` opens a multi-line block.

### Quality
- 58 golden tests pass bit-for-bit (53 prior + 5 new: struct, enum, try/finally,
  operators, null-safe/lambda). Clean under ASan + UBSan.
- Benchmark note: `fib(30)` ≈ 354 ms vs CPython ≈ 133 ms — call-heavy code is
  still slower than CPython. Closing that gap (computed-goto dispatch,
  NaN-boxing, inline caches) is the v0.8 objective, not this release's scope.

## v0.6.0 — bytecode VM

The tree-walking interpreter is retired. Lume now compiles to bytecode and runs on
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
- `use math` / `use x as y` / `use x: a, b` / `use "file.lm"` — built-ins load only when invited.
- Modules are frozen maps; user `.lm` files are modules too (cached, cycle-detected,
  importer-relative paths).

### Built-in Modules
- `math`: lerp, clamp, remap, wrap, move_toward, dist, snap, full trig, exp/log, PI/TAU.
- `game`: Penner easing, pick/pick_weighted/shuffle (loot), signals, poll-based timers.
- `strings`: split, join, Turkish-aware upper/lower, trim, replace, count, padding, chr/ord, fixed.
- `file`: text/lines/binary/JSON/CSV read-write, exists, list_dir, make_dir, delete_file.
- `os`: env, set_env, platform, cwd, args, path_join.

### Packages (RFC-007)
- `lume install user/repo` fetches libraries into `lume_libs/`; `use <name>` imports
  installed packages exactly like built-ins.

### Quality
- Located, helpful English error messages; syntax errors prevent execution entirely.
- 53 golden-file tests, benchmark suite, ASan/UBSan clean.
