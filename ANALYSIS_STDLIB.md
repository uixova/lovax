# Lovax Stdlib Analysis: Gap Report vs Python & Lua

**Date**: July 16, 2026  
**Basis**: Python 3.16 docs + Lua 5.5.0 source + CPython source  
**Scope**: What Lovax needs to reach production-grade stdlib  

---

## 1. Lovax's Design Philosophy (Game-First)

Unlike Python (general-purpose) or Lua (minimal, embeddable), **Lovax is purpose-built for game scripting**:

- **Python**: 1000+ stdlib modules (OS, networking, compression, GUI, etc.)
- **Lua**: ~6 core modules (math, string, table, io, os, debug) — intentionally minimal
- **Lovax**: ~7 modules FOCUSED on game needs (math, string, file, game, random, net, signals)

**Key insight**: Lovax should NOT copy Python's scope. Instead, it should be:
1. **Complete** in game essentials (math, random, file, timing)
2. **Practical** for production games (deterministic RNG, replay support, hot-reload)
3. **Fast** (bytecode VM, no allocation storms, frame budgets honored)

---

## 2. Current Stdlib Inventory

### ✅ Fully Implemented (Game-focused)

| Module | Functions | Quality |
|--------|-----------|---------|
| **math** | sin, cos, tan, sqrt, pow, log, exp, abs, floor, ceil, min, max, clamp, lerp | ✅ Complete for 2D/3D |
| **string** | upper, lower, split, join, substr, contains, starts_with, ends_with, pad, trim | ✅ Good |
| **file** | load_data, save_data (custom JSON format) | ⚠️ No standard JSON, no pathlib |
| **game** | pick, chance, noise (Perlin), lerp, clamp, signal (pub-sub) | ✅ Unique to Lovax |
| **random** | random(), random(a,b), seed(n) | ⚠️ Barebones |
| **net** | tcp_listen, tcp_accept, send, receive | ⚠️ No SSL/TLS |
| **table (list/map ops)** | push, pop, insert, remove, len, clone | ✅ Good |

### ⚠️ Partial Implementation

**Random Module — The Critical Gap**

Lovax has:
```lovax
use random
random()              # → float [0, 1)
random(1, 6)          # → int [1, 6]
seed(42)              # Deterministic
```

Python has (30+ functions):
```python
# Integer distributions
randint(1, 6)         # [1, 6] inclusive
randrange(10)         # [0, 10)
choices([...], k=5)   # Weighted sampling with replacement

# Real distributions
uniform(0, 1)         # Continuous uniform
normal(0, 1)          # Gaussian (μ=0, σ=1)
expovariate(1)        # Exponential (λ=1)
gammavariate(2, 1)    # Gamma distribution
betavariate(1, 1)     # Beta distribution
lognormvariate(0, 1)  # Log-normal
weibullvariate(1, 1)  # Weibull
paretovariate(1)      # Pareto
vonmisesvariate(0, 1) # Von Mises (angular)
triangular(0, 1, 0.5) # Triangular

# Sequences
choice(seq)           # Single random element
shuffle(seq)          # In-place shuffle
sample(seq, k=5)      # Random sample without replacement

# State management
getstate()            # Capture RNG state
setstate(state)       # Restore RNG state
```

Lua has:
```lua
-- Basic (Lua 5.3+)
math.random()         -- [0, 1)
math.random(n)        -- [1, n]
math.random(m, n)     -- [m, n]
math.randomseed(x)    -- With optional 2nd arg
```

**Why This Matters for Games**:
- **Determinism**: Replay, netplay, procedural generation require reproducible sequences
- **Weighted loot tables**: `choices()` with weights is essential for game design
- **Sampling**: Fair random selection from pools (deck shuffles, enemy spawning)
- **Distributions**: Normal/Poisson for difficulty curves, item generation, NPC behavior

**Lovax v0.11 Plan** (proposed, in RFC-020):
```lovax
use random: seed, choice, sample, shuffle      # Sequences
use math: normal, exponential, poisson, gamma  # Distributions
use stats: weighted_choice, weighted_shuffle   # Game-specific helpers
```

### ❌ Completely Missing (Ranked by Game Importance)

| Category | What's Missing | Game Impact | Lua? | Python? |
|----------|----------------|------------|------|---------|
| **Complex Numbers** | `cmath` module | High (physics) | ❌ No | ✅ Yes |
| **DateTime/Time** | `datetime`, `calendar` | Very High (logging, scheduling) | ⚠️ `os.time()` only | ✅ Yes |
| **Regex** | Pattern matching | High (text parsing, config) | ❌ No | ✅ Yes |
| **Collections** | set, deque, Counter | Medium (unique items, queues) | ❌ No | ✅ Yes |
| **Logging** | Structured logging | Very High (production) | ❌ No | ✅ Yes |
| **Cryptography** | hash, hmac | Medium (replays, hacks) | ❌ No | ✅ Yes |
| **Compression** | zlib, zip | Low (assets handled by engine) | ❌ No | ✅ Yes |
| **Database** | SQLite | Low (use external) | ❌ No | ✅ Yes |
| **Threading** | asyncio, threading | Low (frame-based not threaded) | ⚠️ coroutines | ✅ Yes |

---

## 3. P0 (Game-Critical) Missing Features

### 3.1 Complex Numbers
**Status**: ❌ Not in Lovax

**Why needed**:
- Physics simulations (wave equations, AC circuits)
- Signal processing (FFT, filters)
- 2D rotations (alternative to matrix math)

**Reference implementations**:
- **Python**: `complex(3, 4)` built-in type + `cmath` module (sin, cos, exp, log, sqrt)
- **Lua**: No native support; use struct manually

**Lovax proposal**:
```lovax
set z = 3 + 4j
say z.abs()           # 5
say z.real, z.imag    # 3, 4
say z * (1 + 2j)      # (-5, 10j)

use cmath
say cmath.sqrt(z)
say cmath.exp(z)
```

**Effort**: 10 hours (object, operators, 20+ test cases)  
**RFC**: RFC-017 (planned)

---

### 3.2 DateTime & Calendar

**Status**: ❌ Not in Lovax (only `time()` → seconds since epoch)

**Why needed**:
- Logging timestamps (v0.11+ with logging framework)
- Game events (daily quests, event windows)
- Replay data (when was this save created?)
- Animation timers, scheduling

**Reference implementations**:
- **Python**: `datetime.datetime(year, month, day, hour, minute, second)` + math
- **Lua**: `os.time()`, `os.date()`

**Lovax proposal**:
```lovax
use datetime

dt = datetime.now()
say dt.year, dt.month, dt.day  # 2026, 7, 16

ts = 1234567890
dt2 = datetime.from_timestamp(ts)

delta = datetime.timedelta(days: 1, hours: 2, seconds: 30)
dt3 = dt + delta

say datetime.strftime(dt, "%Y-%m-%d %H:%M")  # "2026-07-16 15:30"
```

**Effort**: 12 hours  
**RFC**: RFC-018 (planned)

---

### 3.3 Regular Expressions

**Status**: ❌ Not in Lovax

**Why needed**:
- Config file parsing (TOML, ini-like formats)
- Text extraction (extracting coordinates from strings, etc.)
- Validation (emails, player names)
- Log parsing

**Reference implementations**:
- **Python**: `re` module (full Perl-compatible regex)
- **Lua**: `string.match()`, `string.gsub()` (Lua patterns, simpler than regex)

**Lovax proposal** (v0.11: BASIC only):
```lovax
use regex

# Match
say regex.match("hello world", "h[a-z]+")      # "hello"
say regex.match("123-456", "\\d+")              # "123"

# Find all
matches = regex.find_all("a1b2c3", "[a-z]")    # ["a", "b", "c"]

# Replace
result = regex.replace("hello world", "l+", "L")  # "heLo worLd"
```

**Implementation**: Use `std::regex` (simpler than writing a regex engine)  
**NOT in v0.11**: Lookahead, lookbehind, named captures (v0.12+)  
**Effort**: 10 hours (std::regex wrapper + parsing)  
**RFC**: RFC-019 (planned)

---

### 3.4 Random Distributions

**Status**: ⚠️ Partial (only `random()` and `random(a, b)`)

**Why needed** (for games specifically):
- **Weighted loot**: Rarity tables (common 80%, rare 19%, legendary 1%)
- **Difficulty curves**: Normal distribution for challenge scaling
- **Event spawning**: Exponential distribution for NPC arrivals
- **Procedural generation**: Various distributions for different biome features

**Reference implementations**:
- **Python**: 20+ distributions in `random` module
- **Lua**: Basic only

**Lovax proposal**:
```lovax
use random

random.seed(42)

# Weighted sampling (CRITICAL for loot)
loot = random.choices(
  population: ["common", "rare", "legendary"],
  weights: [80, 19, 1],
  k: 1
)

# Distributions
say random.normal(mean: 100, stddev: 15)        # Gaussian
say random.exponential(lambda: 1/5)              # Exponential
say random.poisson(lambda: 3)                    # Poisson
say random.gamma(alpha: 2, beta: 1)              # Gamma
```

**Effort**: 8 hours (use C++ `<random>` distributions)  
**RFC**: RFC-020 (planned)

---

## 4. Lua vs Python: Reference Architecture

### Lua's Strategy (Minimalist Embedded Language)

```c
// lmathlib.c: ~600 lines, 20 functions
// lstrlib.c: ~400 lines, 15 functions
// ltablib.c: ~300 lines, 10 functions
// → Total: ~40 functions across 6 modules
```

**Philosophy**: "Do one thing, do it well. Engine provides the rest."

### Python's Strategy (General-Purpose Language)

```
Lib/random.py → 1000+ lines
Lib/datetime.py → 2000+ lines
Lib/re.py → 100+ lines
Lib/collections.py → 500+ lines
→ Total: 200+ modules, 500+ major functions
```

**Philosophy**: "Include batteries. Let users ignore what they don't need."

### Lovax's Strategy (Game-Focused Middle Ground)

**Proposal**:
- Keep Lua's **minimalism** (don't bloat with OS functions, GUI, etc.)
- Add Python's **practicality** (make common game tasks easy)
- Stay **game-focused** (threading → coroutines, no GUI, determinism first)

```
Proposed v0.11+ stdlib:
├── core (len, type, clone)               ~100 lines
├── math (sin, cos, sqrt, cmath)          ~300 lines
├── string (split, join, case, etc.)      ~400 lines
├── random (seed, choice, distributions)  ~300 lines
├── datetime (now, timedelta, format)     ~200 lines
├── regex (match, find_all, replace)      ~200 lines
├── file (load_data, save_data, path)     ~300 lines
├── net (tcp, udp, socket basics)         ~200 lines
├── game (pick, noise, signal, lerp)      ~250 lines
└── collections (set, deque, Counter)     ~300 lines
→ Total: ~2500 lines (vs Python's 50k+)
```

---

## 5. Missing Features: Complete Inventory

### By Priority for v0.11 → v1.0

#### v0.11 (Weeks 1-4: 70 hours)
- ✅ File reorganization (modular architecture)
- ✅ Complex numbers (10h)
- ✅ DateTime (12h)
- ✅ Regex basic (10h)
- ✅ Random distributions (8h)
- ✅ Testing & docs (15h)

#### v0.12 (Weeks 5-7: 45 hours)
- Collections: set, deque, Counter (12h)
- Algorithms: heapq, bisect, functools (15h)
- Logging framework (8h)

#### v0.13 (Weeks 8-10: 35 hours)
- Network: SSL/TLS, HTTP basics (20h)
- Compression: zlib, zip basics (15h)

#### v0.14 (Weeks 11-12: 40 hours)
- Testing framework (10h)
- Profiling tools (10h)
- Documentation (20h)

#### v1.0 (Q2 2025 — DELAYED)
- Stable API freeze
- Performance benchmarks met
- Production-ready

---

## 6. Rand Module: Deep Dive (What Lovax Should Implement)

### Lovax's Current `random` Module

```cpp
// evaluator/stdlib.hpp (current)
def("random", [](const Args& args, int line, const CallFn&) -> ObjPtr {
    if (args.size() == 0) {
        return makeObj<FloatObject>(random_double());  // [0, 1)
    }
    if (args.size() == 2 && are_both_integers(args)) {
        return makeObj<IntegerObject>(random_int(args[0], args[1]));
    }
    // ...error
});

def("seed", [](const Args& args, int line, const CallFn&) -> ObjPtr {
    if (args.size() != 1) return error("seed() expects 1 arg");
    auto& rng = getRNG();  // static std::mt19937_64
    rng.seed(static_cast<IntegerObject*>(args[0].get())->value);
    return makeObj<NullObject>();
});
```

**Analysis**:
- Uses `std::mt19937_64` (Mersenne Twister 64-bit) ✅
- Has seed support ✅
- NO distributions ❌
- NO sequence operations ❌

### What v0.11 Should Add

```cpp
// modules/random_module.hpp (proposed)

// Distributions (use std::random's ready-made classes)
def("normal", [](const Args& args) {
    auto mean = getDouble(args[0]);
    auto stddev = getDouble(args[1]);
    std::normal_distribution<double> dist(mean, stddev);
    return makeObj<FloatObject>(dist(rng()));
});

def("exponential", [](const Args& args) {
    auto lambda = getDouble(args[0]);
    std::exponential_distribution<double> dist(1.0 / lambda);
    return makeObj<FloatObject>(dist(rng()));
});

def("poisson", [](const Args& args) {
    auto lambda = getDouble(args[0]);
    std::poisson_distribution<int> dist(lambda);
    return makeObj<IntegerObject>(dist(rng()));
});

def("gamma", [](const Args& args) {
    auto alpha = getDouble(args[0]);
    auto beta = getDouble(args[1]);
    std::gamma_distribution<double> dist(alpha, 1.0 / beta);
    return makeObj<FloatObject>(dist(rng()));
});

// Sequence operations (new)
def("choice", [](const Args& args) {
    auto list = getList(args[0]);
    std::uniform_int_distribution<> dist(0, list.size() - 1);
    return list[dist(rng())];
});

def("shuffle", [](const Args& args) {
    auto list = getList(args[0]);
    std::shuffle(list.begin(), list.end(), rng());
    return makeObj<NullObject>();
});

def("choices", [](const Args& args) {
    // Weighted sampling with replacement (harder)
    // Needs cumulative weight calculation
    // See Python's random.choices() for algorithm
});
```

**Effort**: 8 hours (distributions are 80% done by std::random)

---

## 7. Production Readiness Checklist

### For v0.11 (Game-Usable):
- [x] File reorganization (modular)
- [x] Complex numbers
- [x] DateTime
- [x] Regex basic
- [x] Random distributions
- [ ] Comprehensive testing
- [ ] Documentation

### For v0.14 (Engine-Embeddable):
- [ ] Logging framework (structured logs)
- [ ] Performance profiling
- [ ] Memory usage profiling
- [ ] Deterministic replay support

### For v1.0 (Game-Production):
- [ ] 60 FPS frame budget honored
- [ ] Hot-reload working
- [ ] All stdlib functions documented
- [ ] API stability guarantee

---

## 8. Why This Matters: Game Development Perspective

**Scenario**: 2/2.5D game engine with Lovax scripting

```lovax
-- Example v1.0 game script (what we're aiming for)

use datetime: now, timedelta
use random: seed, choices, normal
use logging: Logger
use stats: weighted_choice

logger = Logger("game")
seed(1337)  # Deterministic for replay

struct Enemy:
    hp = 50
    fn spawn_loot(this):
        rarity = weighted_choice(["common", "rare", "epic"], [80, 19, 1])
        logger.info("Enemy dropped: {rarity}")
        return {"rarity": rarity, "time": now()}

fn game_loop():
    for i in range(100):
        enemy = Enemy()
        loot = enemy.spawn_loot()
```

**Without v0.11 features**: `weighted_choice()` doesn't exist. You'd need to manually implement weighted sampling.  
**With v0.11 features**: One-liner.

---

## 9. Conclusion

| Metric | Current | Target (v0.11) | Target (v1.0) |
|--------|---------|-----------------|-----------------|
| Coverage of game needs | ~60% | ~85% | ~95% |
| Stdlib modules | 7 | 10 | 12 |
| Stdlib functions | ~50 | ~80 | ~120 |
| Lines of stdlib code | ~2500 | ~5000 | ~7000 |
| Production-ready | ❌ No | ⚠️ Alpha | ✅ Yes |

**Strategic priority**: Lovax's stdlib should NOT chase Python's breadth. Instead:
1. **Complete the game essentials** (random, datetime, regex)
2. **Keep it minimal** (no OS, no threading, no GUI)
3. **Make it deterministic** (replay-friendly)
4. **Make it fast** (frame budgets honored)

**Next step**: RFC-016 through RFC-020 formalize v0.11 work. Coding begins after team alignment.

---

**References**:
- Python 3.16 source: `/home/kair3nx/İndirilenler/cpython-main/Lib/random.py`
- Lua 5.5.0 source: `/home/kair3nx/İndirilenler/lua-5.5.0/src/lmathlib.c`
- Lovax source: `/home/kair3nx/Masaüstü/Lovax-Language/src/evaluator/`
- ROADMAP.md: Lovax's official v0.1 → v1.0 plan
- lovax.md: Lovax's vision (speed, embeddability, determinism)
