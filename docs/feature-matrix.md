# Lovax feature matrix — Tier 1 + Tier 2 closure (v0.14)

The measurable definition of "no gap remains": every Tier 1/Tier 2 row from
the Python-3.16/Lua-5.5 gap analysis (ANALYSIS_*.md, COMPREHENSIVE_ANALYSIS
Part 5), with its Lovax answer. Tier 3 (SQLite, compression, crypto, SSL/HTTP,
async) is intentionally OUT — the foundations (bytes type, one-file module
registry, socket layer) keep the door open without shipping dead code.

## Tier 1 (essential)

| Gap-analysis item | Lovax answer | Status |
|---|---|---|
| tuple type | `(a, b)`, `(5,)`, `()`; multi-return `return a, b`; unpack `set q, r = f()`; immutable | ✅ v0.13 |
| set type | `Set([1,2,3])`; `\| & -` operators, `in`, order-insensitive `==`, add/remove/has/values | ✅ v0.13 |
| bytes type | `bytes([..])/bytes(text)`; index→int, slice, `text()` decode; file/net integrated | ✅ v0.13 |
| complex numbers | `3 + 4j` literal, full arithmetic, `.real/.imag`, `use cmath` | ✅ v0.13 |
| regex (basic) | `use regex` — own step-limited engine; match/search/find_all/replace/split/groups | ✅ v0.14 |
| datetime | `use datetime` — now/make/add/diff/format/parse/weekday, own calendar math | ✅ v0.14 |
| random distributions | `use random` — normal/exponential/poisson/gamma/beta/triangular + choice/shuffle/sample/choices, own deterministic impls | ✅ v0.14 |
| format strings | `strings.fmt(x, ".2f"/"0Nd"/"x")` + interpolation `{x}` | ✅ v0.14 (full `{x:.2f}` spec mini-language: v2) |
| collections module | `counter`, `deque` (O(1) both ends), `setdefault` | ✅ v0.14 |
| OrderedDict | unnecessary — Lovax maps preserve insertion order | ✅ by design |
| namedtuple | unnecessary — `struct` (compact slot instances) | ✅ by design |
| defaultdict | `get(m, k, default)` + `collections.setdefault` | ✅ equivalent |

## Tier 2 (important)

| Gap-analysis item | Lovax answer | Status |
|---|---|---|
| itertools | `use iters` — chain/product/combinations/permutations/accumulate/takewhile/dropwhile/groupby/repeat/zip_longest | ✅ v0.14 (eager; lazy = v2 with generators) |
| heapq / bisect | `collections.heap_push/heap_pop/heapify` (min-heap, `[priority, payload]` pairs) + `collections.bisect` (+ core `binary_search`) | ✅ v0.16 |
| functools | `use functools` — partial, memoize; `reduce` is core | ✅ v0.14 |
| logging | `use log` — debug/info/warn/error, set_level, to_file, timestamps | ✅ v0.14 |
| glob / pathlib-style | `file.glob(pattern)`, `file.walk(dir)`, path_join/basename/dirname/extension | ✅ v0.14 |
| subprocess (basic) | `os.run(cmd)` → `{code, out}`, gated by `--allow-run` | ✅ v0.14 |
| JSON in-memory | `use json` — parse/text (same core as save_data/load_data) | ✅ v0.14 |
| exception hierarchy | error **kinds**: structured `throw` + `e.kind` convention + `error(kind, msg)` (RFC-022) | ✅ v0.15 |
| testing framework | `use testing` — assert_eq/assert_true/assert_error + `summary()` | ✅ v0.15 |
| type hints | separate tooling release (with LSP), pre-engine plan keeps it out of v0.14 | ⏳ deliberate |
| HTTP client (minimal) | Tier-3 boundary: socket layer + bytes are the foundation; module lands post-engine if needed | ⏳ deliberate |
| string extras | partition ✅, encode/decode = `bytes(text)`/`text(bytes)` ✅, is_alpha/is_digit/is_space ✅ (existing), casefold = Turkish-aware upper/lower ✅ | ✅ |
| math extras | pi/tau/e/inf/nan, log10/log2, hypot, copysign, is_finite, prod, gcd/lcm/factorial (existing) | ✅ v0.14 |

Every row above is EXECUTE-PROVEN: `tests/cases/57-analiz-dogrulama.lov`
runs 55 assertions covering each ✅ line via the testing module, as a golden
test in the release gate.

## Explicitly out (Tier 3 — bloat by our own analysis)

SQLite, zlib/zip, cryptography, SSL/TLS, threading/asyncio, GUI, ML.
Extension points documented in RFC-018 (bytes) and the module registry:
each would be a one-file module without touching the VM.

## Uniquely Lovax (not in the gap lists)

Deterministic cross-platform RNG (replay promise), Turkish-aware casing,
capability sandbox (`--sandbox --allow-*`), game module (Perlin noise,
weighted pick, signals), coroutines, `--mem-stats`, version-pinned packages.
