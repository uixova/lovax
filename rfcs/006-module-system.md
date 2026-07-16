# RFC-006: Module System — `use`, Dot Access, Stdlib Organization

**Status:** ACCEPTED (implemented)

## Motivation
An early version injected ~45 library functions into the global scope — the language felt
crowded. Decision: libraries ship built-in but must be *invited* with a one-line import,
users can add their own libraries, and added libraries must never break the language.

Research: the Lua model (module = table, explicit require, table = namespace) is the
simplest proven design; Python's selective `from x import y` is added for ergonomics.

## Decision 1: `use`
```
use math                  # binds the module object -> math.lerp(0, 10, 0.5)
use math as m             # alias
use math: lerp, clamp     # selective: names come into scope directly
use "tools/inventory.lov"          # user file module -> inventory.<fn>
use "tools/inventory.lov" as inv
use "tools/inventory.lov": restock
```
- `as` and `:` cannot be combined (ambiguous).
- File modules load once (cache); circular `use` errors with the chain shown;
  paths resolve relative to the IMPORTING file.
- A file module exports everything defined at its top level (functions + variables).

## Decision 2: Dot (`.`) member access
- Modules are represented as maps; `math.lerp` is member access.
- BONUS: the same operator works on string-keyed maps: `player.hp -= 25` (GDScript feel);
  `player["hp"]` remains valid.
- Module maps are FROZEN: `math.lerp = 5` is an error — the language cannot be broken.
- Reading a missing member is an error that lists the available names.

## Decision 3: Stdlib organization
**Core (always available — the language itself):** say, ask, len, text, num, kind, push,
pop, insert, remove, clear, keys, values, has, merge, range, contains, find, slice, reverse,
sort, sort_by, sum, each, filter, transform, abs, min, max, floor, ceil, round, sqrt, pow,
random, seed, clock, sleep, check, copy, clone.

**Built-in modules (via use):** `math`, `game`, `strings`, `file`, `os` — see README.

## Decision 4: File formats
- JSON (save_data/load_data), CSV (quoted-field aware), plain text/lines, raw bytes
  (read_bytes/write_bytes for .bin-style assets).
- PDF/binary media formats do NOT enter the language core — wrong layer. The v1.0 engine
  embedding API will let C++ plugins expose native modules to Lovax; images/audio/PDF live there.
- Process execution (exec/system) is deliberately excluded from the core.
