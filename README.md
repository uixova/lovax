<div align="center">

# 🔆 Lovax

**A programming language that aims to be as simple as Python and as fast as C++ — built as the scripting language for an upcoming 2 / 2.5D game engine.**

Written from scratch in modern C++17, zero dependencies, single-command build.

*Current version: v1.1.0 — true division (`/`) with a new `//` floor operator, single-binary bundling (`lovax bundle`), a three-tier JIT (register allocator → tracing compiler → template compiler, own zero-dependency x86-64 encoder), unboxed inline struct fields, exact int64 on an 8-byte NaN-boxed value, and a C++/C-ABI embedding bridge (RFC-025) for hosting Lovax inside an engine. Game loops now trace to native SSE. Honest speed baseline below · [Türkçe aşağıda ⬇](#-türkçe)*

</div>

---

## Why Lovax?

- **Python-simple syntax** — indentation blocks, no semicolons, readable keywords.
- **Game-first design** — weighted loot tables, signals/events, easing curves, timers, save files and deterministic RNG are *built into the language*, not third-party packages.
- **Clean core, invited libraries** — built-in modules never pollute your scope until you `use` them.
- **Friendly errors** — located, human-readable, and they tell you the fix.
- **Full UTF-8** — identifiers like `oyuncu_adı` are first-class; `len("şey")` is 3, not 6.

```lovax
use game: pick_weighted, signal, connect, emit
use file

set player = {"name": "Kai", "hp": 100}

set damaged = signal()
fn on_damage(amount):
    player.hp -= amount
connect(damaged, on_damage)
emit(damaged, 25)

set reward = pick_weighted({"common": 80, "rare": 19, "legendary": 1})
say "HP: {player.hp}, you found: {reward}"
file.save_data("save.json", player)
```

## New in v0.7 — a real core

```lovax
struct Player:
    hp = 100
    name = "hero"
    fn hurt(amount):          # 'this' is implicit
        this.hp -= amount
    fn status():
        return "{this.name}: {this.hp} hp"

enum State: IDLE, WALK, ATTACK

set p = Player(80, "Aria")
p.hurt(30)
say p.status()                # Aria: 50 hp
say State.ATTACK              # 2

try:
    set data = file.load_data("save.json")
catch e:
    say "no save yet: {e}"
finally:
    say "boot complete"

set weapon = p.weapon?.name ?? "bare hands"   # null-safe + coalesce
set double = fn(x) -> x * 2                    # arrow lambda
```

Run `lovax` with no arguments for an interactive **REPL**. Full rationale for what
earned a keyword (and what was rejected) is in
[RFC-009](rfcs/009-rich-core.md); `struct`/`enum` in [RFC-010](rfcs/010-struct-enum.md),
error handling in [RFC-008](rfcs/008-error-handling.md).

## New in v0.10 — general-purpose & safe

```lovax
use net                                  # TCP/UDP sockets: servers, clients, LAN, VPS
set server = net.tcp_listen(8080)
set client = net.tcp_accept(server)
net.send(client, "pong")
```

```bash
lovax install someuser/inventory@v1.2.0   # version-pinned: locks the exact commit in lovax.lock
lovax --sandbox --allow-net app.lov        # run untrusted code: net yes, filesystem/env no
```

Coroutines (`yield`/`spawn`/`resume`), a capability **sandbox** (Deno-style
`--allow-*`), and version-pinned packages make Lovax safe to run other people's
code — see [RFC-014](rfcs/014-coroutines.md) and [RFC-015](rfcs/015-net-and-sandbox.md).

## Quick Start

**Install (prebuilt binary — no compiler needed):**

```bash
curl -fsSL https://raw.githubusercontent.com/uixova/lovax/main/install.sh | sh   # Linux / macOS
```
```powershell
irm https://raw.githubusercontent.com/uixova/lovax/main/install.ps1 | iex        # Windows
```

Then `lovax update` self-updates to the newest release. Prefer to build from source?
Zero dependencies — just a C++17 compiler:

```bash
make               # optimized ./lovax  (or: make dev / make asan / make test)
make install       # copy ./lovax to ~/.local/bin

# no make? one command still works:
g++ -std=c++17 -O3 -fno-gcse -fno-crossjumping -o lovax src/main.cpp
```

**Run:**

```bash
lovax                            # interactive REPL
lovax examples/hello_world.lov
lovax examples/dungeon.lov        # loot table + signals + save system
lovax --version
```

**Ship a single binary (RFC-027):**

```bash
lovax bundle game.lov -o game    # one self-contained executable — no install needed
./game arg1 arg2                  # runs the embedded script; argv reach it via os.args()
```

`bundle` copies the interpreter and appends your script as a trailer, so an end
user runs the result with nothing else installed — the model Deno `compile` and
Bun `--compile` use. A plain `lovax` binary has no trailer and behaves as usual.

## Language Tour

### Variables — `set` defines, bare assignment updates

```lovax
set speed = 10      # DEFINES a new variable
speed = 20          # UPDATES an existing one
speed += 5          # compound: += -= *= /= //= %=

score = 1           # ERROR! 'score' is not defined (typo protection)
```

This rule (RFC-001) closes Python's "a typo silently creates a new variable" trap,
and closures update outer counters naturally — no `nonlocal` ceremony.

### Printing & strings

```lovax
say "Hello", 100, true               # comma-separated values
say "hp: {hp}, doubled: {hp * 2}"    # interpolation: any expression inside {}
say "escaped: \n \t \" \{literal\}"

set dialogue = """Hello traveler!
{gold} gold awaits you below."""     # multiline strings (game dialogue)
```

`"text" + 5` is deliberately an error (no implicit conversion) — use `text(5)` or interpolation.

### Control flow

```lovax
if age > 65:
    say "retired"
else if age >= 18 and has_license:
    say "can drive"
else:
    say "pedestrian"

match state:
    "running":
        say "active"
    "paused", "waiting":     # multiple patterns (OR)
        say "idle"
    _:                       # wildcard
        say "unknown"

set label = "low" if x < 5 else "high"    # ternary expression

while hp > 0:
    hp -= 10
    if hp == 50:
        break

for i in range(10, 0, -2):    # 10 8 6 4 2 (end exclusive)
    say i
for item in ["sword", "shield"]:
    say item
for key in some_map:
    say key, some_map[key]
```

### Functions

```lovax
fn heal(target, amount = 10):        # default parameters (evaluated at call time)
    return "{target} +{amount} hp"

fn counter():                        # closures
    set n = 0
    fn bump():
        n += 1
        return n
    return bump

set is_even = fn(n):                 # anonymous functions
    return n % 2 == 0
say filter([1, 2, 3, 4], is_even)    # [2, 4]
```

Strict arity: wrong argument counts are errors. Infinite recursion produces a clean
error (call-depth limit 500) instead of a segfault.

### Data types & operators

| Type | Example | Notes |
|------|---------|-------|
| `int` | `42`, `0xFF`, `0b1010`, `1_000_000` | 64-bit; `/` is true division (always float: `7 / 2 == 3.5`), `//` is floor division consistent with floor `%`: `(a // b) * b + a % b == a` |
| `float` | `3.14`, `1e6`, `2.5e-3` | 64-bit, scientific notation |
| `string` | `"hi"`, `"""multi\nline"""` | UTF-8; indexing/length by code point |
| `bool`, `null` | `true`, `false`, `null` | |
| `list` | `[1, "two", [3]]` | negative indexing, `+` concatenation |
| `map` | `{"k": 1}` | insertion-ordered; dot access: `player.hp` |
| `range` | `range(0, 10)` | lazy, allocates nothing |

| Category | Operators |
|----------|-----------|
| Arithmetic | `+ - * / // % **` (`/` true division → float; `//` floor division → int; `**` right-assoc: `2 ** 3 ** 2` = `512`) |
| Comparison | `== != < > <= >=` (deep equality: `[1,[2]] == [1,[2]]`) |
| Logic | `and or not` (short-circuit; `x or default` idiom works) |
| Membership | `in` — `2 in [1,2]`, `"a" in "cat"`, `"k" in map`, `4 in range(0,10,2)` |
| Bitwise | `& \| ^ ~ << >>` (ints only, Python precedence) |
| Assignment | `= += -= *= /= //= %=` |

Truthiness (Python model): `null`, `false`, `0`, `0.0`, `""`, `[]`, `{}` → false.

### Modules — `use` (RFC-006)

Built-in libraries ship with the language but stay out of your way until invited:

```lovax
use math                    # module object -> math.lerp(0, 10, 0.5)
use math as m               # alias
use game: pick_weighted     # selective: name comes into scope directly
use "tools/weapons.lov"      # YOUR library -> weapons.compute_damage(...)
use "tools/weapons.lov" as w
```

- File modules load once (cached); circular `use` is caught with a clear error;
  paths resolve relative to the importing file.
- A file module exports everything at its top level (functions + variables).
- Modules are **frozen**: `math.lerp = 5` is an error — libraries can't break the language.
- Discovery: `keys(math)` works; accessing a missing member lists what exists.

### Packages — `lovax install` (RFC-007)

Install community libraries with one command and import them by name:

```bash
lovax install someuser/inventory      # GitHub shorthand -> lovax_libs/inventory/
```

```lovax
use inventory                        # imports like a built-in
say inventory.restock("potion")
```

`use <name>` resolves built-ins first, then `lovax_libs/<name>/<name>.lov` (or `main.lov`).
Publishing a package = pushing a repo with a `<name>.lov` at its root — nothing else.
A manifest (`lovax.json`) with version pinning and a registry are planned (see
[rfcs/007-packages.md](rfcs/007-packages.md)).

### Built-in functions

**Core (no import needed):**

| Group | Functions |
|-------|-----------|
| Basics | `say` `ask` `len` `text` `num` `kind` |
| Lists/Maps | `push` `pop` `insert` `remove` `clear` `keys` `values` `has` `merge` `range` |
| Collections | `contains` `find` `slice` `reverse` `sort` `sort_by` `sum` |
| Higher-order | `each(l, fn)` `filter(l, fn)` `transform(l, fn)` |
| Math | `abs` `min` `max` (also take a list) `floor` `ceil` `round` `sqrt` `pow` |
| Misc | `random` `seed` `clock` `sleep` `check` (assert) `copy` (shallow) `clone` (deep) |

**Built-in modules (via `use`):**

| Module | Contents |
|--------|----------|
| **`math`** | `lerp` `clamp` `remap` `sign` `wrap` `move_toward` `dist` `snap` `sin` `cos` `tan` `asin` `acos` `atan2` `exp` `log` `deg` `rad` + `PI` `TAU` |
| **`game`** | `ease(t, "out_bounce")` (Penner set) · `pick` `pick_weighted` `shuffle` (loot) · `signal` `connect` `disconnect` `emit` (events) · `timer` `timer_done` `timer_left` `timer_reset` (cooldowns) |
| **`strings`** | `split` `join` `upper` `lower` (Turkish-aware) `trim` `replace` `starts_with` `ends_with` `count` `pad_start` `pad_end` `chr` `ord` `fixed` |
| **`file`** | `exists` `read_text` `write_text` `append_text` `read_lines` `read_bytes` `write_bytes` (binary) `delete_file` `make_dir` `list_dir` · `save_data`/`load_data` (JSON) · `read_csv`/`write_csv` |
| **`os`** | `env` `set_env` `platform` `cwd` `args` `path_join` |

> Binary formats beyond raw bytes (PDF, images, audio) belong to the engine plugin layer
> (planned v1.0 embedding API), not the language core (RFC-006).

### Errors

Errors are located, human-readable, and suggest the fix. **A file with syntax errors
never runs** (multiple errors reported in one pass). Exit codes: `0` ok, `64` usage, `65` syntax, `70` runtime.

```
[Syntax Error] line 2, column 7: expected '=' but got integer
[Runtime Error] line 5: undefined variable 'x' (define it with: set x = ...)
```


## Project Layout

```
src/            # the whole language: lexer, parser, AST, bytecode compiler, VM, stdlib
examples/       # runnable sample scripts
tests/          # golden-file test runner + 52 cases (tests/tmp/ is scratch, gitignored)
benchmarks/     # fib / mandelbrot / game-loop measurements
rfcs/           # design documents behind every language decision
```

For your own projects any layout works; a simple convention:

```
my_game/
├── main.lov         # entry: ./lovax main.lov
└── libs/           # your libraries -> use "libs/inventory.lov"
```

Module caching is in-memory per run — no cache files are ever written to disk.

## Tests & Benchmarks

```bash
./tests/run_tests.sh              # 66 golden-file tests (features + errors + stress)
./tests/fuzz.sh                   # security gate: adversarial + fuzz, no crashes allowed
./tests/run_tests.sh --update     # regenerate expected outputs (verify diffs first!)
./benchmarks/run_benchmarks.sh
```

## Performance — honest baseline

Lovax compiles to bytecode and runs on a direct-threaded stack VM (computed-goto
dispatch), and hot loops are then compiled to native x86-64 by a **three-tier
JIT** (all zero-dependency, our own encoder — no LLVM, no DynASM):

1. a **register allocator** takes pure-int local loops,
2. a **tracing compiler** takes float / global / struct-field loops (runtime-typed,
   floats in XMM, per-access type guards that side-exit on a surprise), and
3. a **template compiler** is the always-correct fallback.

Struct fields are unboxed inline values (an `entity.x` write allocates nothing),
and math builtins (`sqrt`, `floor`, `ceil`, `abs`) emit SSE inside a trace.

Because Lovax is a **game** language, the benchmarks that matter are entity/update
loops, not generic recursion. Same machine, outputs verified identical, best-of-3
(ms, lower is better; full harness in [benchmarks/cross/](benchmarks/cross/)):

| Game loop | Lovax | LuaJIT | Node | what it is |
|-----------|------:|-------:|-----:|-----------|
| entity_update | **39** | 8 | 42 | struct-of-arrays float update (beats Node) |
| aos_update | **49** | 23 | — | array-of-structs `e = ents[j]; e.x += e.vx` |
| vec_math | **56** | 22 | — | `sqrt(dx*dx+dy*dy)` distance accumulation |
| particles | **97** | 42 | — | per-frame spawn/update/despawn churn |

The game loops trace to native and land **2–5× off LuaJIT** (and Lovax wins on
startup — a small static binary, ~5 ms). LuaJIT is the reference to learn from,
not a target to match feature-for-feature — it is a mature general-purpose JIT,
Lovax is a young game-first one. Generic recursion (`fib`) still trails; that is a
deliberately low priority. The remaining game lever is hoisting per-iteration
bounds/range checks out of the loop (ABC + LICM). See
[benchmarks/cross/RESULTS.md](benchmarks/cross/RESULTS.md) for the full read,
including the generic microbenches.

## Architecture & Roadmap

`Lexer -> Parser (Pratt) -> AST -> Compiler -> Bytecode -> Stack VM -> JIT`.
The language surface is stable (118 golden tests pin every behavior — including
error messages — cross-checked against the interpreter across every build axis).
Milestones:

1. ~~VM phase 2 — computed goto, superinstructions, call fast path~~ **done (v0.8)**.
2. ~~Coroutines, string speed, inline cache, one-line install~~ **done (v0.9)** ([RFC-014](rfcs/014-coroutines.md)).
3. ~~Tracing GC + 16-byte value~~ **done (v0.11)** ([RFC-013](rfcs/013-value-representation.md)).
4. ~~Compact/unboxed struct fields, incremental GC (≤1 ms pauses), stdlib gaps (set, tuple, bytes, regex, random distributions)~~ **done (v0.12–v0.16)**.
5. ~~8-byte NaN-boxed value (exact int64 kept) + C++/C-ABI embedding bridge for engines~~ **done (v0.18 / v1.0, [RFC-025](rfcs/025-embed-ffi.md))**.
6. **Now (v1.x JIT):** three-tier native compilation is live (register allocator + tracing + template, own x86-64 encoder). Next: hoist per-iteration bounds/range checks (ABC + LICM) and a young-generation nursery for per-frame allocation churn.

See [lovax.md](lovax.md) for the deep performance research and [rfcs/](rfcs/) for design decisions.

## License

MIT — see [LICENSE](LICENSE).

---

<a name="-türkçe"></a>
<details>
<summary><h2>🇹🇷 Türkçe</h2></summary>

**Lovax**, Python kadar sade söz dizimine sahip, C++ ile sıfırdan yazılmış, ileride
geliştirilecek bir 2/2.5D oyun motorunun ana betik dili olacak bir programlama dilidir.

### Neden Lovax?

- **Python sadeliği** — girinti tabanlı bloklar, noktalı virgül yok.
- **Oyun-öncelikli** — loot tabloları (`pick_weighted`), sinyaller (`signal/connect/emit`),
  easing eğrileri, zamanlayıcılar, kayıt sistemi (`save_data/load_data`) ve deterministik
  rastgelelik (`seed`) dilin içinde gelir.
- **Temiz çekirdek** — gömülü kütüphaneler (`math`, `game`, `strings`, `file`, `os`)
  `use` ile davet edilmeden kapsamına girmez; modüller donmuştur, dil bozulamaz.
- **Türkçe dostu** — `oyuncu_adı` gibi tanımlayıcılar birinci sınıftır; `len("şey") == 3`;
  `upper/lower` Türkçe kurallıdır (`i→İ`, `ı→I`); hata mesajları satır numarasıyla çözümü söyler (küresel standart için İngilizcedir).

### Hızlı Başlangıç

```bash
make                                      # optimize edilmiş ./lovax (veya: make dev / make test)
./lovax examples/turkish_showcase.lov     # Türkçe tanımlayıcı vitrini
./lovax examples/dungeon.lov
./lovax                                   # argümansız: etkileşimli REPL
```

### Öne çıkan kurallar

- `set x = 5` **tanımlar**, çıplak `x = 5` **var olanı günceller** (yazım hatası koruması, RFC-001).
- `"metin" + 5` bilerek hatadır; `text(5)` veya `"toplam: {x}"` interpolasyonu kullanılır.
- `%` ve `/` taban (floor) kurallıdır: `(a / b) * b + a % b == a` her zaman doğrudur.
- `match` ilk eşleşen dalı çalıştırır, düşme yoktur; `_` jokerdir.
- Kendi kütüphaneni yaz: `use "libs/envanter.lov"` — bir kez yüklenir, döngüsel `use`
  net hatayla yakalanır.

Testler: `./tests/run_tests.sh` (66 golden test) + `./tests/fuzz.sh` (güvenlik kapısı). Paket kurma: `lovax install kullanıcı/repo` → `use paket_adı`. Tasarım kararları: [rfcs/](rfcs/).

</details>

<div align="center">
<sub>Lovax — write simply, run fast. 🔆</sub>
</div>
