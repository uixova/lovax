# RFC-008 — Error handling: try / catch / throw / finally

## Motivation

Before v0.7 a runtime error was terminal: it unwound the whole program. Real
programs (loading a save file, parsing user input, a failing network package)
need to *recover*. Lume adds structured error handling that stays Python-simple
and reads left-to-right.

## Surface syntax

```lume
try:
    set data = read_file("save.json")
    say data
catch e:
    say "could not load: {e}"
finally:
    say "attempt finished"
```

- `try:` — a block whose runtime errors are catchable.
- `catch <name>:` — runs when the try block raises; `<name>` binds the error
  message as a string. Required (there is no bare `try`).
- `finally:` — optional; runs on both the normal path and the caught path.
- `throw <expr>` — raises `<expr>` (any value; stringified for the catch binding).

Design choices:
- One `catch` per `try` (no typed catch clauses). Match on the message inside the
  block if you need to branch — keeps the surface tiny.
- `finally` is for cleanup that must happen regardless of success. In v0.7 it runs
  on the normal exit and the caught exit; a `catch` that itself re-throws, or a
  `return` out of `try`/`catch`, bypasses it. This is documented and will be
  tightened when the VM grows an unwind table (v0.8).

## Implementation

A handler stack in the VM (`handlers_`), parallel to the call-frame stack:

- `TRY_PUSH <catch-offset>` records `{frameDepth, stackTop, catchIp}`.
- `TRY_POP` discards the innermost handler on normal completion.
- `THROW_` / any catchable runtime error calls the `tryHandle` helper: if a
  handler exists, the VM truncates the value stack and call frames back to the
  recorded depth, pushes the error message string, and jumps to `catchIp`;
  otherwise the error is returned as terminal (unchanged v0.6 behaviour).

The `VM_THROW` macro replaced the bare `return runtimeError(...)` at every
catchable site, so the same error messages now become catchable without changing
their text. `finally` is compiled as a plain block emitted after the catch join
point, so it is reached by both fall-through paths.

## Compatibility

Programs that never use `try` are byte-for-byte unaffected: uncaught errors still
terminate with the identical message and exit code 70.
