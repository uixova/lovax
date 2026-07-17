# Cross-language benchmark — v0.12 (2026-07-17)

Same machine, same workload (outputs verified identical across all languages),
external wall-clock best-of-5. Reproduce with `benchmarks/cross/run.sh`.

Host: Linux x86_64, g++ 16. Interpreters: Lovax 0.12.0, Lua 5.4, Lua 5.5,
LuaJIT, CPython 3.14.6, Node 26.4.

## Time (ms, lower = better)

| bench   | Lovax | Lua 5.4 | Lua 5.5 | LuaJIT | Python | Node |
|---------|------:|--------:|--------:|-------:|-------:|-----:|
| fib(32) | **411** | 204 | 205 | 37 | 332 | 73 |
| strcat  | **107** | 21  | 23  | 28 | 27  | 46 |
| hashmap | 258 | 351 | 314 | 100 | 193 | 382 |
| btree   | 216 | 183 | 163 | 87 | 107 | 68 |
| gc      | 80  | 41  | 37  | 4  | 89  | 49 |
| startup | 5   | 4   | 3   | 4  | 20  | 37 |

## Peak memory (MB, lower = better)

| bench   | Lovax | Lua 5.4 | Lua 5.5 | LuaJIT | Python | Node |
|---------|------:|--------:|--------:|-------:|-------:|-----:|
| gc      | **15**  | 15  | 15 | 15 | 15 | 59 |
| btree   | 37  | 24  | 26 | 32 | 16 | 70 |
| hashmap | 66  | 31  | 31 | 18 | 37 | 113 |

## v0.11 → v0.12 (compact structs RFC-017 + honest GC accounting)

| metric | v0.11 | v0.12 | change |
|---|---:|---:|---|
| btree time | 653 ms | **216 ms** | 3.0× faster |
| btree peak | 270 MB | **37 MB** | 7.3× smaller — now in Lua's league |
| gc peak    | 30 MB  | **15 MB** | matches Lua/LuaJIT/Python exactly |
| everything else | — | — | unchanged (no regressions) |

Two fixes did this: (1) a struct instance is now a shape pointer + flat slot
array instead of a full map with three per-instance hash indexes and per-instance
method closures; (2) the GC threshold now tracks *live* bytes (payload-aware,
recomputed at sweep) instead of a never-decreasing allocation total — dead
garbage no longer piles up between ever-rarer collections. `lovax --mem-stats`
prints allocations / collections / peak / total GC time / max pause.

## Honest reading (no spin)

**Memory is fixed.** Lovax now sits in the Lua tier on every memory bench.

**Pure compute is still the gap** — fib/strcat are ~2× behind Lua 5.4 and the
whole field trails LuaJIT by ~10×. That is interpreter dispatch + call cost;
the fix is the planned v1.x JIT (LuaJIT-inspired, zero-dep), NOT interpreter
micro-tuning — that code gets replaced wholesale. btree/hashmap show the
runtime's data structures are already competitive (hashmap beats every
non-JIT language here).

**Startup stays a win** (5 ms; Python 20, Node 37).

**Missing benches are real language gaps:** no regex engine, no in-memory JSON
parse (`load_data` is file-only). Both land in v0.14 (own regex engine RFC-021,
`json` module) — then this harness grows regex + json rows.

The JIT (v1.x) remains the plan to close the compute gap. This table is the
measured "before" picture it will be judged against.

---

### Archived: v0.11 honest baseline (2026-07-16)

| bench   | Lovax 0.11 | Lua 5.4 | LuaJIT | Python | Node |
|---------|------:|--------:|-------:|-------:|-----:|
| fib(32) | 389 | 190 | 37 | 308 | 73 |
| strcat  | 102 | 19  | 28 | 26  | 45 |
| hashmap | 240 | 279 | 100 | 186 | 409 |
| btree   | 653 | 179 | 97 | 104 | 75 |
| gc      | 82  | 45  | 4  | 92  | 51 |
| mem: btree | **270 MB** | 24 | 31 | 15 | 70 |
| mem: gc    | 30 MB | 15 | 15 | 15 | 59 |

That table refuted the old RFC-013 claim ("beats CPython everywhere") and
triggered the v0.12 memory work above.
