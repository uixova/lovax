# RFC-004: `else if` and the `match` Statement

**Status:** ACCEPTED (implemented)

## Motivation
Lume is not a Python clone; its syntax should read as plain English. `elif` is a
Python-specific contraction; `else if` is shared by C/C#/JS/GDScript and reads naturally.
GDScript's most-loved feature, `match`, flattens long if-chains.

## Decision 1: `else if`
`elif` does not exist. The chain is written `else if cond:` — the parser nests it as an
inner if.

## Decision 2: `match`
```
match state:
    "waiting", "paused":
        say "idle"
    5:
        say "five"
    _:
        say "other"
```
- A pattern is any expression, compared at runtime with deep equality (`==`).
- Comma separates alternative patterns (OR). `_` is the wildcard/default branch.
- First matching branch runs; NO fallthrough. If nothing matches, nothing happens.
- `break/continue/return` propagate out of match bodies normally.
