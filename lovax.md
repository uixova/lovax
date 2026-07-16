# Lovax — Vision, Architecture, and the Road to Speed

> This document explains *where Lovax is going*. For everyday usage and syntax, see
> [README.md](README.md). The goal here: pin down the long-term targets, the technical
> decisions, and a deep answer to **"how will a scripting language get this fast?"**

---

## 1. Vision

Lovax in one sentence: **"A language as easy to write as Python, running close to C++
speed, as the primary language of a 2/2.5D game engine."**

Three pillars:

1. **Ergonomics** — indentation-based, noise-free syntax. A designer or a beginner
   should write game logic without knowing the language internals.
2. **Performance** — being a scripting language is no excuse. The end goal is to beat
   interpreted languages (CPython, classic Lua) by a wide margin in hot loops and to
   compete with JIT-class runtimes (LuaJIT).
3. **Embeddability** — Lovax is not a standalone app; it is a runtime that will be
   embedded into an upcoming 2/2.5D game engine calling Lovax code every frame.

When these conflict, the priority order is: **ergonomics, then performance, then
feature richness.** A fast language that hurts to write slows game development down.

---

## 2. Where We Are

Lovax compiles to **bytecode running on a stack VM** (the tree-walker is retired):

```
.lov  →  Lexer  →  Parser (Pratt)  →  AST  →  Compiler  →  Bytecode  →  Stack VM
```

The language surface is rich and frozen: conditionals (`if/else if`, `match`, ternary),
loops (`while`, `for/in`, `break/continue`), functions (closures, default parameters,
anonymous functions), list/map/range, string interpolation, `in` and `**` operators,
the module system (`use`) and the built-in `math`/`game`/`strings`/`file`/`os` libraries —
all documented in the README and protected by 53 golden tests.

This phase proves **correctness**, not speed. Speed comes from the journey in §4.

---

## 3. Design Decisions

| Decision | Why |
|----------|-----|
| **Indentation blocks** | Python's readability; no brace noise. The lexer emits INDENT/DEDENT. |
| **Explicit `set` definition** | Stops typos from silently creating variables (RFC-001). |
| **English keywords, UTF-8 identifiers** | Keywords stay universal (`if`, `while`, `fn`); variables can be Turkish (`hız`, `düşman_menzilde`). |
| **C++17, header-only** | Zero dependencies, one-command build, easy engine embedding. |
| **`shared_ptr` object model** | Memory safety for the prototype — a known performance debt (§4), replaced later. |

---

## 4. The Performance Journey

### 4.0 Current bottlenecks (accepted, temporary debts)

1. **Per-value heap allocation** — even the integer `5` is `make_shared` with atomic
   refcounting. Allocation storms kill tight loops.
   *Fix:* stack-carried values — tagged union, then **NaN-boxing**.
2. **String-keyed variable access** — every lookup hashes a string through a scope chain.
   *Fix:* a resolver pass mapping variables to **slot indices** at compile time.
3. **Pointer-chasing AST dispatch** — nodes scattered on the heap; cache-hostile.
   *Fix:* a linear bytecode array (§4.2).

### 4.1 Stage I — sharpen the tree-walker *(done)*

NodeType-tag switch instead of dynamic_cast chains; shared true/false/null singletons;
`-O2`/LTO builds. Gains: 2-5×, still within interpreted-language limits.

### 4.2 Stage II — bytecode compiler + VM *(phase 1 shipped: stack VM, immediate values, closed upvalues — mandelbrot beats CPython 3.14; phase 2: register + NaN-boxing)*

Compile the AST into compact bytecode executed by a tight dispatch loop, like Lua,
Python, Wren, and C# do.

- **Register-based VM** (the Lua 5 model): executes >46% fewer instructions than a
  stack VM; case studies show up to 3× ([Lua 5.0 paper](https://www.lua.org/doc/jucs05.pdf),
  [ACM VM showdown](https://dl.acm.org/doi/10.1145/1328195.1328197)).
- **NaN-boxing values**: every value in 64 bits — doubles as-is, other types in NaN bit
  patterns. Twice the values per cache line; used by LuaJIT and JavaScriptCore.
- **Computed-goto dispatch**: branch-predictor-friendly threaded code (CPython, YARV);
  +20-50% by itself.
- **Slot-resolved variables, constant folding, own call frames** (deep Lovax recursion
  can no longer overflow the C++ stack — and coroutines become "park the frame stack").
- Expected: **10-100×** over the tree-walker; the golden test suite is the
  behavior-equivalence insurance for the migration.

### 4.3 Stage III — game-friendly memory

Frame budget is sacred (60 FPS = 16.6 ms). Strategy layers: allocate less (NaN-boxed
values never touch the heap), **frame arenas** for per-frame temporaries (free = reset),
and an **incremental mark-sweep GC** with write barriers (the Unity/Lua model) targeting
≤1 ms pauses. The current `shared_ptr` refcounting (and its cycle leaks) retires here.

### 4.4 Stage IV — JIT / AOT

- **JIT**: hotness counters per function; compile hot paths natively (LuaJIT's secret) —
  via LLVM ORC or a light custom backend (Luau's selective-JIT model).
- **AOT**: transpile Lovax to C++/LLVM-IR for shipped games — "release build" at native
  speed while development keeps the interpreted hot-reload loop.

| Stage | Model | Relative speed |
|-------|-------|----------------|
| now | tree-walker (sharpened) | 1× |
| II | bytecode + VM | 10-100× |
| III | game-friendly memory | (no pauses) |
| IV | JIT / AOT | ~C++ |

---

## 5. Beyond the VM

- Coroutines (`wait`/`yield`) once the VM owns call frames.
- `struct` + optional type hints (RFC-003) — hints later feed JIT specialization.
- Tooling: REPL, tree-sitter grammar, LSP, `lovax fmt`.
- Distribution: one-command installer (`curl | sh`, Windows installer), GitHub-Releases
  CI binaries, `lovax update` self-update with stable/latest channels.
- Docs site: one page per language element with runnable examples (doc examples double
  as golden tests, so documentation can never go stale).

## 6. Engine Integration (v1.0)

- Embedding API: the engine registers C++ functions as native Lovax modules
  (`draw`, `play_sound`, `collides`); binary formats (images/audio/PDF) live in this
  plugin layer, not the language core.
- Lifecycle contract: optional `start()` / `update(dt)` per script, called by the engine.
- Hot-reload (≤100 ms) and determinism guarantees (seeded RNG, ordered execution) for
  replays and networking.

## 7. Positioning

| Language | Strength | Why Lovax differs |
|----------|----------|------------------|
| Lua/LuaJIT | tiny, JIT-fast | C-flavored syntax; Lovax aims for Python simplicity + UTF-8 identity |
| GDScript | engine-integrated | tied to Godot; Lovax targets its own engine with a C++ core |
| Wren | elegant class-based VM | great architecture reference; Lovax is game-API-first |
| Python | easiest syntax | heavy to embed and slow; Lovax wants "light Python feel + native speed" |

---

<div align="center">
<sub>Write simply, run fast. Long road, clear map. 🔆</sub>
</div>
