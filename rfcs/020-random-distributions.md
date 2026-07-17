# RFC-020 — random: deterministic distributions & sampling

`use random` → `uniform, normal, exponential, poisson, gamma, beta,
triangular`; `choice, shuffle, sample, choices(list, weights, k)` (weighted
loot tables); `getstate/setstate` (replay capture); `seed`.

**The decision that matters:** every distribution is OUR implementation
(Box-Muller, Knuth's product method, Marsaglia-Tsang squeeze, inverse CDF)
over exactly two primitives built on the raw mt19937_64 stream:

- `rngU53()` — 53-bit float in [0,1)
- `rngBounded(n)` — Lemire multiply-shift bounded int

`std::uniform_*_distribution` results differ between libstdc++/libc++/MSVC.
A game replay seeded with `seed(42)` must reproduce bit-identically on every
platform — Lovax's determinism promise — so std::random distributions are
banned from the runtime. The global `random()/random(a,b)` and
`game.pick/pick_weighted/shuffle/chance` were moved onto the same two
primitives (behavior change within a seed, cross-platform stability gained).

Algorithms studied from the literature and CPython's Lib/random.py — studied,
never copied.
