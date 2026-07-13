# RFC-014 — Coroutines

## Motivation

Game logic is full of "do this, wait, then do that": cutscenes, AI behaviour
trees, timed sequences, staged spawns. Writing those as state machines by hand
is painful. Coroutines — functions that pause and resume — are the natural tool,
and Lua proved the model for games.

## Surface

```lume
fn counter(start):
    set n = start
    while true:
        yield n        # pause, hand n to resume(); resume value becomes this expr
        n += 1

set c = spawn(counter)   # a paused coroutine (status: created)
say resume(c, 10)        # 10   (first resume value is the fn argument)
say resume(c)            # 11
say resume(c)            # 12
say co_status(c)         # "suspended"
```

- `yield <expr>` — pauses the running coroutine and hands `<expr>` to `resume()`.
  It evaluates to the value passed to the *next* `resume()`, so data flows both
  ways. `yield` alone yields null.
- `spawn(fn)` — creates a coroutine from a function (not yet running).
- `resume(co, [value])` — runs it until the next `yield` or `return`; returns the
  yielded/returned value. The first resume passes its value as the function's
  argument (if the function declares one).
- `co_status(co)` — `"created"` / `"suspended"` / `"running"` / `"dead"`.
- `co_done(co)` — true once the coroutine has returned.

An error thrown inside a coroutine surfaces at the `resume()` call (catchable)
and marks the coroutine dead. Resuming a dead or running coroutine is an error.

## Implementation

The VM already keeps its call frames in an explicit stack (not the C stack), so a
coroutine is just a **saved execution context**:

```
struct ExecState { stack; spOffset; frames; openUpvalues; handlers; };
```

- Each coroutine (and the main program) owns one `ExecState`.
- `resume` saves the caller's context, loads the coroutine's, and calls `run()`
  (a nested C call). `run()` executes until:
  - a `YIELD_` op — sets a `yielded_` flag, saves ip into the frame, and returns
    out of `run()`; `resume` then re-saves the coroutine's context and hands back
    the yielded value; or
  - the coroutine function returns — its result is on top of its own stack;
    `resume` pops it and marks the coroutine dead; or
  - an error — surfaced to `resume`, coroutine marked dead.
- Two-way values: `yield` pops the produced value; on the next `resume` the
  resume argument is pushed so it becomes the yield expression's result.

Because coroutines reuse the existing frame/upvalue machinery, closures captured
across a `yield`, nested coroutines (a coroutine resuming another), function
calls inside a coroutine, and `try/finally` around a `yield` all work with no
special cases. Verified under ASan + UBSan + LeakSanitizer, including 100
abandoned suspended coroutines (each holding its own stack) with no leaks.

## Limitations / future

- `wait(seconds)` is intentionally **not** built in: real time belongs to the
  host. The engine integration (v1.0) drives suspended coroutines each frame via
  a scheduler; `yield` is the primitive that makes `wait` expressible there.
- Each suspended coroutine currently reserves a full value stack. A future pass
  can right-size or pool these once the GC lands (RFC-013).
