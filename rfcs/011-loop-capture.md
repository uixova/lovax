# RFC-011 — Loop-variable capture semantics

## Question

What should a closure created inside a `for` loop capture — the loop variable's
*final* value (Python), or *that iteration's* value (most other languages)?

```lume
set fns = []
for i in 0..3:
    push(fns, fn() -> i)
say "{fns[0]()},{fns[1]()},{fns[2]()}"
```

## Survey (Lume is a game language — look past Python)

| Language | Result | Model |
|----------|--------|-------|
| Python | `2,2,2` | one binding, late-bound at call time |
| C# (`foreach`, since C# 5) | `0,1,2` | fresh binding per iteration |
| JavaScript (`let`) | `0,1,2` | fresh binding per iteration |
| Lua | `0,1,2` | loop variable is a fresh local each pass |
| GDScript / Godot | `0,1,2` | fresh per iteration |
| Swift, Kotlin, Rust | `0,1,2` | fresh per iteration |

Python is the outlier, and its late binding is a well-known footgun — it bites
hardest in exactly the pattern game code writes constantly:

```lume
for enemy in enemies:
    connect(enemy.died, fn() -> drop_loot(enemy))   # each handler needs ITS enemy
```

Under Python semantics every handler would drop loot for the *last* enemy.

## Decision

**Lume uses per-iteration capture (`0,1,2`).** The loop variable is a fresh
binding each pass. Other variables captured from an *outer* scope stay shared
references — only the loop variable is per-iteration:

```lume
set total = 0
for n in 0..4:
    push(gs, fn() -> n)   # per-iteration: 0,1,2,3
    total += n            # 'total' is shared: every closure sees the final 6
```

The loop variable still leaks after the loop (`for k in …: …` then `say k`),
matching Python and C# — convenient, and orthogonal to capture.

## Implementation

The loop variable is a real local slot in every scope, **including the top-level
script frame** (so script-level and function-level loops behave identically). At
the end of each iteration a `CLOSE_UPVALUE` op snapshots any upvalues that
captured the loop variable before the slot is reused for the next element — the
standard technique (Lua, *Crafting Interpreters*). Enabling script-frame locals
also means a closure defined at top level can capture a top-level loop variable,
which Python-style globals could not express.
