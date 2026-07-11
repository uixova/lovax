# RFC-001: `set` Semantics — Definition vs. Assignment

**Status:** ACCEPTED (implemented)

## Motivation
In Python, a typo like `sped = 5` silently creates a new variable and hides bugs for hours.
In Lua, forgetting `local` accidentally creates a global. Both traps should be closed.

## Decision
- `set x = 5` → **defines** in the current scope (overwrites if present).
- `x = 5` (bare assignment) → walks the scope chain and **updates an existing** variable;
  if it exists nowhere, error: `tanımlanmamış değişken 'x' (tanımlamak için: set x = ...)`.
- Compound assignments (`x += 5` etc.) follow the bare-assignment rule.

## Consequences
+ Typos are caught immediately.
+ Closures work naturally: an inner function's `n += 1` updates the outer counter
  (no `nonlocal` ceremony).
- Two concepts to learn (set vs =) — an accepted cost; the error message teaches the fix.

## Alternatives
- Python model (assignment = definition): rejected due to the typo trap.
- Mandatory `set` on every assignment: noisy, breaks the natural `i += 1` pattern; rejected.
