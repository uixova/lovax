<div align="center">

# 🔆 Lume

**A programming language that aims to be as simple as Python and as fast as C++ — built as the scripting language for an upcoming 2 / 2.5D game engine.**

Written from scratch in modern C++17, zero dependencies, single-command build.

*Current version: v0.8.0 — the speed release: faster than CPython · [Türkçe aşağıda ⬇](#-türkçe)*

</div>

---

## Why Lume?

- **Python-simple syntax** — indentation blocks, no semicolons, readable keywords.
- **Game-first design** — weighted loot tables, signals/events, easing curves, timers, save files and deterministic RNG are *built into the language*, not third-party packages.
- **Clean core, invited libraries** — built-in modules never pollute your scope until you `use` them.
- **Friendly errors** — located, human-readable, and they tell you the fix.
- **Full UTF-8** — identifiers like `oyuncu_adı` are first-class; `len("şey")` is 3, not 6.

```lume
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

```lume
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

Run `lume` with no arguments for an interactive **REPL**. Full rationale for what
earned a keyword (and what was rejected) is in
[RFC-009](rfcs/009-rich-core.md); `struct`/`enum` in [RFC-010](rfcs/010-struct-enum.md),
error handling in [RFC-008](rfcs/008-error-handling.md).

## Quick Start

**Install (prebuilt binary — no compiler needed):**

```bash
curl -fsSL https://raw.githubusercontent.com/uixova/lume/main/install.sh | sh   # Linux / macOS
```
```powershell
irm https://raw.githubusercontent.com/uixova/lume/main/install.ps1 | iex        # Windows
```

Then `lume update` self-updates to the newest release. Prefer to build from source?
It's one command, zero dependencies:

```bash
g++ -std=c++17 -O3 -fno-gcse -fno-crossjumping -o lume src/main.cpp
```

**Run:**

```bash
lume                            # interactive REPL
lume examples/hello_world.lm
lume examples/dungeon.lm        # loot table + signals + save system
lume --version
```

## Language Tour

### Variables — `set` defines, bare assignment updates

```lume
set speed = 10      # DEFINES a new variable
speed = 20          # UPDATES an existing one
speed += 5          # compound: += -= *= /= %=

score = 1           # ERROR! 'score' is not defined (typo protection)
```

This rule (RFC-001) closes Python's "a typo silently creates a new variable" trap,
and closures update outer counters naturally — no `nonlocal` ceremony.

### Printing & strings

```lume
say "Hello", 100, true               # comma-separated values
say "hp: {hp}, doubled: {hp * 2}"    # interpolation: any expression inside {}
say "escaped: \n \t \" \{literal\}"

set dialogue = """Hello traveler!
{gold} gold awaits you below."""     # multiline strings (game dialogue)
```

`"text" + 5` is deliberately an error (no implicit conversion) — use `text(5)` or interpolation.

### Control flow

```lume
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

```lume
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
| `int` | `42`, `0xFF`, `0b1010`, `1_000_000` | 64-bit; `/` is floor division, consistent with floor `%`: `(a / b) * b + a % b == a` |
| `float` | `3.14`, `1e6`, `2.5e-3` | 64-bit, scientific notation |
| `string` | `"hi"`, `"""multi\nline"""` | UTF-8; indexing/length by code point |
| `bool`, `null` | `true`, `false`, `null` | |
| `list` | `[1, "two", [3]]` | negative indexing, `+` concatenation |
| `map` | `{"k": 1}` | insertion-ordered; dot access: `player.hp` |
| `range` | `range(0, 10)` | lazy, allocates nothing |

| Category | Operators |
|----------|-----------|
| Arithmetic | `+ - * / % **` (`**` right-assoc: `2 ** 3 ** 2` = `512`) |
| Comparison | `== != < > <= >=` (deep equality: `[1,[2]] == [1,[2]]`) |
| Logic | `and or not` (short-circuit; `x or default` idiom works) |
| Membership | `in` — `2 in [1,2]`, `"a" in "cat"`, `"k" in map`, `4 in range(0,10,2)` |
| Bitwise | `& \| ^ ~ << >>` (ints only, Python precedence) |
| Assignment | `= += -= *= /= %=` |

Truthiness (Python model): `null`, `false`, `0`, `0.0`, `""`, `[]`, `{}` → false.

### Modules — `use` (RFC-006)

Built-in libraries ship with the language but stay out of your way until invited:

```lume
use math                    # module object -> math.lerp(0, 10, 0.5)
use math as m               # alias
use game: pick_weighted     # selective: name comes into scope directly
use "tools/weapons.lm"      # YOUR library -> weapons.compute_damage(...)
use "tools/weapons.lm" as w
```

- File modules load once (cached); circular `use` is caught with a clear error;
  paths resolve relative to the importing file.
- A file module exports everything at its top level (functions + variables).
- Modules are **frozen**: `math.lerp = 5` is an error — libraries can't break the language.
- Discovery: `keys(math)` works; accessing a missing member lists what exists.

### Packages — `lume install` (RFC-007)

Install community libraries with one command and import them by name:

```bash
lume install someuser/inventory      # GitHub shorthand -> lume_libs/inventory/
```

```lume
use inventory                        # imports like a built-in
say inventory.restock("potion")
```

`use <name>` resolves built-ins first, then `lume_libs/<name>/<name>.lm` (or `main.lm`).
Publishing a package = pushing a repo with a `<name>.lm` at its root — nothing else.
A manifest (`lume.json`) with version pinning and a registry are planned (see
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
├── main.lm         # entry: ./lume main.lm
└── libs/           # your libraries -> use "libs/inventory.lm"
```

Module caching is in-memory per run — no cache files are ever written to disk.

## Tests & Benchmarks

```bash
./tests/run_tests.sh              # 53 golden-file tests (features + errors + stress)
./tests/run_tests.sh --update     # regenerate expected outputs (verify diffs first!)
./benchmarks/run_benchmarks.sh
```

## Performance

Lume compiles to bytecode and runs on a direct-threaded stack VM (computed-goto
dispatch), with immediate numeric values, in-place stack arithmetic, fused
superinstructions ([RFC-012](rfcs/012-vm-performance.md)), an in-place string
package, and a member-access inline cache. Same machine, recommended flags,
best-of-5 against **CPython 3.14** and **Lua 5.4**:

| Benchmark | **Lume 0.9** | CPython 3.14 | Lua 5.4 |
|-----------|-------------:|-------------:|--------:|
| string suite (concat/interp/keys/eq) | **19 ms** | 240 ms | — |
| member access (1M struct r/w) | **68 ms** | 108 ms | — |
| `fib(30)` (pure recursion) | 66 ms | 79 ms | **44 ms** |

Lume beats CPython on every workload, and beats Lua 5.4 on string/data work.
The one benchmark Lua still leads is pure recursion — that gap is the value
representation (Lume's value is 32 bytes incl. a refcounted pointer; Lua's is 8).
Closing it is the v0.10 headline (intrusive refcount → NaN-boxing), scoped as its
own effort in [RFC-013](rfcs/013-value-representation.md) rather than rushed.

## Architecture & Roadmap

`Lexer -> Parser (Pratt) -> AST -> Compiler -> Bytecode -> Stack VM`.
The language surface is complete and frozen (53 golden tests pin every behavior,
including error messages). Next milestones:

1. ~~VM phase 2 — computed goto, superinstructions, call fast path~~ **done in v0.8: beats CPython**.
2. ~~Coroutines, string speed, inline cache, one-line install~~ **done in v0.9** ([RFC-014](rfcs/014-coroutines.md)).
3. **v0.10**: value representation (intrusive refcount → NaN-boxing) to pass Lua on recursion; game-friendly GC.
4. Optional type hints; LSP, formatter, VSCode extension.
5. **v1.0**: engine embedding API, hot-reload, determinism guarantees.

See [lume.md](lume.md) for the deep performance research and [rfcs/](rfcs/) for design decisions.

## License

MIT — see [LICENSE](LICENSE).

---

<a name="-türkçe"></a>
<details>
<summary><h2>🇹🇷 Türkçe</h2></summary>

**Lume**, Python kadar sade söz dizimine sahip, C++ ile sıfırdan yazılmış, ileride
geliştirilecek bir 2/2.5D oyun motorunun ana betik dili olacak bir programlama dilidir.

### Neden Lume?

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
g++ -std=c++17 -O3 -fno-gcse -fno-crossjumping -o lume src/main.cpp
./lume examples/turkish_showcase.lm     # Türkçe tanımlayıcı vitrini
./lume examples/dungeon.lm
```

### Öne çıkan kurallar

- `set x = 5` **tanımlar**, çıplak `x = 5` **var olanı günceller** (yazım hatası koruması, RFC-001).
- `"metin" + 5` bilerek hatadır; `text(5)` veya `"toplam: {x}"` interpolasyonu kullanılır.
- `%` ve `/` taban (floor) kurallıdır: `(a / b) * b + a % b == a` her zaman doğrudur.
- `match` ilk eşleşen dalı çalıştırır, düşme yoktur; `_` jokerdir.
- Kendi kütüphaneni yaz: `use "libs/envanter.lm"` — bir kez yüklenir, döngüsel `use`
  net hatayla yakalanır.

Testler: `./tests/run_tests.sh` (53 golden test). Paket kurma: `lume install kullanıcı/repo` → `use paket_adı`. Tasarım kararları: [rfcs/](rfcs/).

</details>

<div align="center">
<sub>Lume — write simply, run fast. 🔆</sub>
</div>
