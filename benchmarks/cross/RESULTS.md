# Cross-language benchmark — honest baseline (2026-07-16)

Same machine, same workload (outputs verified identical across all languages),
external wall-clock best-of-5. Reproduce with `benchmarks/cross/run.sh`.

Host: Linux x86_64, g++ 16. Interpreters: Lovax 0.11.0, Lua 5.4, Lua 5.5,
LuaJIT, CPython 3.14.6, Node 26.4.

## Time (ms, lower = better)

| bench   | Lovax | Lua 5.4 | Lua 5.5 | LuaJIT | Python | Node |
|---------|------:|--------:|--------:|-------:|-------:|-----:|
| fib(32) | **389** | 190 | 191 | 37 | 308 | 73 |
| strcat  | **102** | 19  | 24  | 28 | 26  | 45 |
| hashmap | 240 | 279 | 277 | 100 | 186 | 409 |
| btree   | **653** | 179 | 157 | 97 | 104 | 75 |
| gc      | 82  | 45  | 53  | 4  | 92  | 51 |
| startup | **5** | 4 | 3 | 3 | 24 | 47 |

## Peak memory (MB, lower = better)

| bench   | Lovax | Lua 5.4 | LuaJIT | Python | Node |
|---------|------:|--------:|-------:|-------:|-----:|
| gc      | 30  | 15  | 15  | 15  | 59  |
| btree   | **270** | 24  | 31  | 15  | 70  |
| hashmap | 63  | 31  | 19  | 37  | 113 |

## Honest reading (no spin)

**Lovax is currently the slowest of the group on compute** — fib/strcat/btree
are ~2× behind Lua 5.4, ~1.3× behind CPython, ~10× behind LuaJIT. It is only
competitive on hashmap (beats Lua + Node) and gc (beats Python), and it *wins*
startup decisively (5 ms — small static binary, no runtime init; Python 24 ms,
Node 47 ms).

**This refutes the old RFC-013 claim** ("beats CPython everywhere, beats Lua on
strings"). Those numbers predate the v0.11 tracing GC / 16-byte value work,
which *regressed* both time and memory. The call-cost gap vs Lua widened
(≈1.5× → ≈2×). The gains RFC-013 targeted did not materialize on this machine.

**Memory is the loudest problem.** btree uses **270 MB vs Lua's 24 MB (11×)**.
Root cause: a Lovax `struct` instance is a full `MapObject` (an entries vector
+ three typed indexes strIndex/intIndex/boolIndex) — very heavy per instance —
and the GC's "collect when heap doubles" threshold lets that garbage pile up.

## What this means for the roadmap (see ROADMAP)

1. **The speed thesis is unproven and currently false.** Lovax is a clean, safe,
   fast-*starting* bytecode interpreter at ~CPython tier — behind Lua, far behind
   LuaJIT. "C++-fast" is a v1.x-JIT promise, not a v0.11 fact. Say so publicly.
2. **Compact struct instances** (flat slot array, field→slot resolved at compile
   time — like a Lua table's array part, not a hash map with 3 side indexes) is
   the single biggest win available now: ~10× less struct memory + faster field
   access. Bigger lever than incremental GC.
3. **GC heuristics / incremental GC** (planned) cut the accumulation, but the
   per-object size is the primary memory driver.
4. **Missing benches are real gaps:** no regex, no in-memory JSON parse.

The JIT (v1.x, LuaJIT-inspired, zero-dep) remains the plan to close the compute
gap. This table is the honest "before" picture to measure it against.
