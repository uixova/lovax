# RFC-013 — Value representation: the road to beating Lua on calls

## Where we stand (measured, same machine, best-of-5)

| Benchmark | Lovax v0.9 | CPython 3.14 | Lua 5.4 |
|-----------|----------:|-------------:|--------:|
| string suite | **19 ms** | 240 ms | (concat 64 ms) |
| member 1M | **68 ms** | 108 ms | — |
| `fib(30)` (pure recursion) | 66 ms | 79 ms | **44 ms** |

Lovax beats CPython everywhere and beats Lua on string/data work. The **one**
place Lua still wins is pure recursion (`fib`). This RFC is about closing that.

## Diagnosis

`sizeof(Value) == 32` bytes:

```
struct Value {
    VKind kind;                  // 1 byte (+7 padding)
    union { bool; long long; double; };  // 8 bytes
    std::shared_ptr<Object> obj; // 16 bytes  <-- the weight
};
```

The `shared_ptr` is 16 of the 32 bytes and, more importantly, every push/pop of
an *object* value touches its control block (refcount inc/dec). On `fib` the hot
cost is: each of 2.7M calls pushes the callee closure (an object Value → refcount
traffic) and moves 32-byte Values through the stack. (Atomics are already elided
— libstdc++ sees `__libc_single_threaded == 1` — so it's the copy + control-block
touch, not lock contention.)

Lua's value is **8 bytes** (a tagged union / NaN-boxed), and its GC means values
are copied without any per-move refcount work. That 4×-smaller, refcount-free
value is the whole `fib` gap.

## Options

1. **NaN-boxing** — pack every value into one 64-bit double's bit patterns
   (numbers as themselves, pointers/tags in the NaN space). `sizeof(Value) → 8`.
   Requires object lifetimes to be managed by a **GC** (or intrusive refcount),
   because a NaN-boxed pointer can't carry a `shared_ptr` control block.
2. **Intrusive refcount** — keep refcounting but move the count *into* `Object`
   and use a custom 8-byte smart pointer. `sizeof(Value) → 16`. A smaller step
   that still removes the control-block indirection, and is the natural stepping
   stone to (1).

Both are correct; (2) is the safer first move and de-risks (1).

## Update (v0.11): NaN-boxing rejected for Lovax — int64 conflict

Implementing the value model surfaced a hard constraint: **NaN-boxing an 8-byte
value is incompatible with Lovax's native `int64`.** A NaN-boxed value has only
~48–51 bits of payload, so a full 64-bit integer does not fit. LuaJIT gets away
with NaN-boxing precisely because it has *no* int64 (numbers are doubles, 53-bit
safe). Lua 5.4, which *does* have int64, therefore uses a **16-byte tagged union**
— not NaN-boxing.

So Lovax follows Lua 5.4: a **16-byte tagged `Value`** (tag + `union{int64, double,
Object*}`), down from 24 (Ref era) / 32 (shared_ptr era). This keeps exact int64,
is fully portable (no per-platform pointer-bit assumptions — a real NaN-box
hazard on ARM / 5-level paging), and matches the value size of the int64 peer we
actually compete with (Lua 5.4). Boxing large ints to fake an 8-byte value, or
adding a range check to every int op, would slow the exact hot path (`fib`) we
mean to speed up. Decision recorded; `LOVAX_SAFE_VALUES` is unnecessary since the
16-byte encoding *is* the safe, portable one.

## Historical note: original defer decision (v0.9)

**Deferred to a dedicated v0.10+ "value representation" effort — do NOT squeeze
it in now.** Reasoning:

- It touches essentially every `std::shared_ptr<Object>` in the codebase
  (object model, runtime, builtins, stdlib, VM). It cannot be done incrementally
  behind a fast path the way string/member/coroutine work was.
- The VM is currently fast, fully green (62 golden, both dispatch modes,
  ASan/UBSan/Leak clean). Half-landing a value-model rewrite at the end of a
  session would trade a stable, tested interpreter for an unproven one.
- The wins already banked in v0.9 (string 12×, member 1.6×, coroutines) are the
  bulk of the release's value and are independent of this change.

### Plan for v0.10

1. Land **intrusive refcount** first (Object gets a count; `Rc<Object>` 8-byte
   pointer replaces `shared_ptr`). Full golden + sanitizer gate. Expected: Value
   32 → 16, measurable `fib` gain from removing control-block indirection.
2. Prototype **NaN-boxing** on a branch behind a compile flag; measure `fib`
   against Lua. Ship only if it clears the bar and stays sanitizer-clean.
3. Target: `fib(30)` **≤ 44 ms** (Lua parity), then push past it — the last
   benchmark where Lovax isn't already #1 among dynamic interpreters.

This is a scheduling decision, not a capability gap: the path is understood and
measured; it just deserves its own verification cycle rather than a rushed one.
