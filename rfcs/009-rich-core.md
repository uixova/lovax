# RFC-009 — Rich core: which constructs earn a keyword

## Motivation

A 28-keyword core is too thin: developers hit workarounds for things that other
languages express directly (null-safe access, single-expression lambdas, safe
defaults). The counter-failure is worse — padding the keyword count with words
the VM cannot honestly implement (`unsafe`, `async`, `class`) produces a language
that lies about what it does.

This RFC records the **logic filter** applied to a large batch of suggested
keywords: every rejection is justified, every acceptance fits Lume's architecture
(shared-pointer objects, single-threaded bytecode VM, composition over
inheritance).

## Accepted (v0.7)

| Construct | Form | Why it fits |
|-----------|------|-------------|
| `struct` + `this` | `struct Player: hp = 100 … fn hurt(a): this.hp -= a` | Real tagged data with methods; `player.hp` is a genuine field. See [RFC-010](010-struct-enum.md). |
| `enum` | `enum State: IDLE, WALK, ATTACK` | Named integer constants for state machines / AI. Compiles to a frozen map. |
| `finally` | `try … catch e … finally …` | Cleanup on every path. See [RFC-008](008-error-handling.md). |
| `?.` null-safe access | `player?.weapon?.damage` | Chain through possibly-null links; yields null instead of crashing. |
| `->` arrow lambda | `fn(x) -> x * 2` | Single-expression closure; desugars to `fn(x): return x * 2`. |
| Compound bitwise/coalesce assign | `&= \| = ^= <<= >>= ??=` | Layer masks and safe defaults are one token, matching `+=` family. |

`new` is reserved but unused: instantiation is `Player(...)`, keeping the surface
free of ceremony.

## Rejected (with reason)

- `unsafe` / `fixed` / `stackalloc` / `ref` / `out` — Lume has no manual memory;
  a keyword that cannot do what its name promises is a broken keyword.
- `class` / `base` / `virtual` / `override` / `abstract` / `sealed` / `interface`
  — inheritance was deliberately dropped (composition, RFC-003). `struct` covers
  the real need.
- `elif` — removed in RFC-004 in favour of `else if`. `case` / `default` — `match`
  already gives multi-pattern arms with a `_` wildcard, which is cleaner.
- `async` / `await` — the VM is single-threaded; there is no real asynchrony to
  expose. Cooperative coroutines are a separate, honest feature (v0.8).
- `char` / `typeof` / `as_cast` — a one-character string, `kind()`, and
  `num()` / `text()` already cover these.

## Note on `??` and trigraphs

`tokenTypeName` returns `"'?\?'"` for `QQ`, not `"'??'"`. The escaped `\?`
prevents the C++ trigraph `??'` from being interpreted by the compiler. This is
correct C++; a plain `??` there triggers a `-Wtrigraphs` warning. (A prior review
flagged the escape as a bug — it is not.)

## Outcome

Core keyword count grows ~28 → ~40 *usable* keywords. Count was never the target;
none of the additions are decorative, and each maps to real bytecode.
