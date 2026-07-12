# RFC-010 — struct and enum

## struct

```lume
struct Player:
    hp = 100          # field with a default
    name = "hero"
    weapon = null
    fn hurt(amount):  # method — 'this' is implicit, never declared
        this.hp -= amount
    fn status():
        return "{this.name}: {this.hp} hp"

set p = Player(80, "Aria")   # fields become positional constructor params
p.hurt(30)
say p.status()               # Aria: 50 hp
say p.hp                     # 50
```

### Semantics

- **Fields become constructor parameters**, in declaration order, with their
  defaults preserved. `Player()` uses all defaults; `Player(80, "Aria")` overrides
  the first two.
- **Instances are tagged maps.** A struct value is an insertion-ordered map with a
  hidden `__type__` field holding the struct name. Field access (`p.hp`) and
  mutation (`p.hp = 5`, `this.hp -= n`) are ordinary map operations, so everything
  that already works on maps works on instances.
- **Methods take an implicit `this`.** You never write the receiver in the
  parameter list. Inside a method, `this` is the instance.
- **No inheritance.** Composition only (RFC-003): a `Player` *has a* `Weapon`.

### How a method call knows its receiver

`obj.method(args)` compiles to `MEMBER_GET_KEEP` (keep the receiver) + the args +
`CALL_METHOD`. At runtime `CALL_METHOD` inspects the receiver:

- If it is a struct instance (a map carrying `__type__`), the receiver is passed
  as the implicit first argument `this`.
- Otherwise (a module like `math`, or a plain map with a function field) it is a
  normal call and the receiver is **not** injected — so `math.clamp(a, b, c)`
  keeps its arity.

This one runtime check is what lets method-call syntax and module-call syntax
share the `.` operator without colliding.

### Implementation

`struct` compiles to a factory closure bound to the struct name (and marked
`const`). Calling it builds the instance map: `__type__`, then each field from its
parameter slot, then each method as a closure whose implicit slot 0 is `this`.
Because methods are stored in the map, they are *not* bound as top-level names —
the compiler skips the usual named-function name-binding for methods.

## enum

```lume
enum State: IDLE, WALK, ATTACK      # inline form
enum Dir:                           # block form, one member per line
    NORTH
    EAST
    SOUTH
    WEST

say State.ATTACK   # 2
say Dir.NORTH      # 0
```

Members are assigned consecutive integers starting at 0. An enum compiles to a
map `{ "IDLE": 0, … }` bound `const`, so `State.IDLE` is a normal member access.
Both the comma-separated inline form and the indented block form are accepted.

## Limitations (v0.7)

- No per-field type annotations or validation.
- No custom constructor body — construction is field assignment only. Run an
  `init`-style method after construction if you need derived setup.
- Method closures are created per instance; fine for typical game-object counts,
  revisited if profiling shows it matters.
