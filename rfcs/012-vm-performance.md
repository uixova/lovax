# RFC-012 — VM performance: beating CPython (v0.8)

## Goal and result

The v0.8 target was set early: **beat CPython on call-heavy code**, not just on
numeric loops. Result, best-of-3 wall clock (GCC, recommended flags):

| Benchmark | v0.6 | v0.8 | CPython 3.14 |
|-----------|-----:|-----:|-------------:|
| `fib(30)` — 2.7M calls | 357 ms | **115 ms** (−68%) | 139 ms |
| `heavy_loop` — arith/nested/float | 727 ms | **371 ms** (−49%) | 449 ms |

Recommended build: `g++ -std=c++17 -O3 -fno-gcse -fno-crossjumping`.
(`-fno-crossjumping` stops GCC from merging the per-handler indirect jumps back
into one, which would undo the dispatch win below; `-fno-gcse` is the standard
companion for computed-goto interpreters.)

## What was done, in order of measured impact

### 1. Pass-by-reference `push` (the hidden killer)

`void push(Value v)` took its argument **by value**: every push copy-constructed
a 32-byte Value (with a `shared_ptr` member) into the parameter, then
move-assigned it into the slot — two passes, two branch pairs, on the hottest
function in the VM. Split into `push(const Value&)` / `push(Value&&)`.
This single change took `fib(30)` from 187 ms to 116 ms.

### 2. Fused superinstructions (compiler peephole)

The compiler now emits fused opcodes for the shapes real code actually takes:

- `ADD_I SUB_I MUL_I MOD_I BAND_I BOR_I BXOR_I` — `<expr> op <small int>` in one
  op (`n - 1`, `% 13`, `& 255`); i16 immediate, generic fallback preserves the
  exact unfused error messages.
- `LESS_JF LESS_EQ_JF GREATER_JF GREATER_EQ_JF EQUAL_JF NOT_EQUAL_JF` — branch
  conditions compile to compare-and-jump in one dispatch (`if`, `while`,
  `until` via inverted comparison, ternary, `repeat` counters).
- `LT_I_JF LE_I_JF GT_I_JF GE_I_JF EQ_I_JF NE_I_JF` — comparison against an
  int immediate fused into the branch (`n < 2` is one op after the operand).
- `LGET2` — push two locals in one dispatch (`a + b` shape).
- `LGET_ADD_I / LGET_SUB_I` — local ± immediate in one op (the `fib(n-1)`
  argument shape).

### 3. In-place stack arithmetic

Binary numeric ops no longer pop two Values and push one (three 32-byte moves).
They operate directly on the stack slots (`sp_[-2] op= sp_[-1]; --sp_`) — zero
Value moves on the int/float path. The object slow path still materializes
Values inside its own scope, keeping error messages identical.

### 4. Direct-threaded dispatch (computed goto)

On GCC/Clang each opcode handler ends with its own indirect jump to the next
handler (`VM_NEXT_FAST`), giving the branch predictor per-handler history
instead of one mega-mispredicting switch. Portable fallback: define
`LUME_NO_COMPUTED_GOTO` to get the plain `switch` (both modes are tested).

**The destructor trap:** `goto *label` is invisible to the compiler's scope
analysis — jumping out of a scope with live non-trivial locals (`Value`,
`shared_ptr`, `string`) skips their destructors and leaks. Discipline:

- `VM_NEXT_FAST` (computed goto) is allowed **only** in handlers with no named
  non-trivial locals in scope. Scalar locals (`int`, `double`, pointers,
  references) are fine.
- `VM_NEXT` (plain `goto` to the dispatch label) runs destructors and is safe
  everywhere; every slow path uses it.
- Verified with a full ASan + LeakSanitizer sweep over all tests and stress
  programs in both dispatch modes.

### 5. Call-path work

- Exact-arity closure calls (no defaults, not variadic — the dominant shape)
  are inlined in the `CALL` handler; the general `callValue` handles the rest.
- `RETURN` moves the result down in place instead of pop-and-repush.
- Frames cache `consts`/`chunk` pointers, so frame switches are flat loads
  instead of chasing two `shared_ptr`s.
- `FOR_NEXT` has an all-scalar fast path for range iteration (the hot loop
  shape `for i in 0..n`).

## Rejected / deferred

- **NaN-boxing** (8-byte values): the biggest remaining lever, but it forces a
  manual refcount or GC for object lifetimes — deep surgery, deferred to v0.9+.
- **Register VM**: superinstructions captured most of the dispatch savings at a
  fraction of the rewrite cost.
- **`-march=native`**: distribution binaries must stay portable.

## Guardrails

Every step was gated on: 60 golden tests bit-for-bit (both dispatch modes),
stress suite green, ASan/UBSan/LeakSanitizer clean. Error messages are part of
the golden contract — fused ops fall back through the same `Runtime` paths so
no message changed.
