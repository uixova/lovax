# RFC-024: 8-byte NaN-boxed Value with exact int64 (hybrid boxed-int)

Status: ACCEPTED (design; implementation lands as v0.18)
Supersedes the value-representation part of RFC-013 (which chose the 16-byte
tagged union and explicitly deferred NaN-boxing).

## Motivation

The 16-byte tagged union (`VKind` + union) is correct, portable and identical
to Lua 5.4's approach. But our performance peer is LuaJIT, not stock Lua, and
studying `lj_obj.h` shows an 8-byte value is a foundation of LuaJIT's speed
*before the JIT even runs*: a value fits ONE register, so interpreter
dispatch, stack traffic, call boundaries and (later) JIT glue code all move
half the bytes. At 16 bytes every Value copy is two registers/two stores —
a permanent tax on every opcode.

Cross-bench evidence: fib/heavy_loop are dispatch+copy bound; v0.17's Track A
(allocator + map index) moved everything allocation-bound but left fib flat,
exactly as predicted. The remaining interpreter-side lever is value width.

## Why LuaJIT's exact scheme is NOT enough for us

`lj_obj.h` (GC64): 13 bits forced NaN + 4-bit itype + 47-bit payload.
Integers exist only as a 32-bit subtype; `setint64V` silently converts a
64-bit int to a double when it doesn't fit 32 bits. Lovax guarantees exact
int64 semantics: our BIT_AND/OR/XOR/SHL/SHR run on full 64-bit `long long`,
game code relies on 64-bit masks/packed IDs/hashes, and goldens encode that
behavior. Dropping int64 is rejected.

## Design: hybrid — 47-bit inline int + heap-boxed rare int64

One `uint64_t` per value.

- **Double**: stored as its own bit pattern. Any REAL double whose bits fall
  in the tag range is impossible: the FPU only produces the canonical quiet
  NaN; every double WRITE canonicalizes NaN inputs to 0x7ff8000000000000
  ("a NaN is a NaN" — payload is not observable in the language today).
- **Tagged values**: top 13 bits all 1 (0xFFF8...), next 4 bits = tag,
  low 47 bits = payload:
  - `NIL`, `FALSE`, `TRUE` — payload unused.
  - `INT47` — payload holds a two's-complement integer in [-2^46, 2^46);
    decode sign-extends from bit 46. Covers indexes, counters, timestamps,
    pixel/coin/score values — in practice nearly every integer a game makes.
  - `OBJ` — payload is the Object* (47-bit user VA on x86-64/arm64 Linux,
    macOS, Windows). GC reads pointers ONLY through the decode helper.
- **int64 outside INT47 range**: allocated as a `BoxedIntObject` (new
  ObjectType::BOXED_INT, pooled size-class 1 — 16..32 B) carried under the
  OBJ tag. Arithmetic fast paths handle INT47×INT47 with overflow checks
  (`__builtin_add_overflow` on the 47-bit range); any operand or result
  outside the range takes the slow path, which computes in native int64 and
  re-tags (inline if it fits, box if not). Semantics stay EXACT: same
  results, same wraparound-free behavior, same golden outputs.
  - Equality/comparison/`kind()`/printing treat BoxedInt as `int`
    transparently: `is int`, `5 == boxed(5)`, map int keys (hash the
    UNBOXED value — a boxed 5 and inline 5 are the same key).

Proven parts, novel only in combination: JSC/SpiderMonkey NaN-box
(double+pointer inline), V8 smi (small int inline, big int boxed),
LuaJIT dualnum (guarded int fast path). 8 bytes + exact int64 together is
our differentiator.

## Platform rule

`LOVAX_NANBOX` is on by default on 64-bit x86-64/arm64. Any platform where
the 47-bit-pointer assumption fails (or any 32-bit target) builds the 16-byte
union instead — the accessor API (below) makes the two layouts drop-in
interchangeable. The interpreter stays the single source of truth either way.

## Implementation plan — two phases, each gated

**Phase 1 — mechanical, zero behavior change (16B stays):** make Value's
fields private; convert all ~150 direct `.kind/.i/.d/.b/.obj` access sites
(vm.hpp ~136, base.hpp, object.hpp, value.hpp) to accessors:
`kind()`, `asInt()`, `asFloat()`, `asBool()`, `asObj()`, `setObj()`, etc.
Gate: golden 83/83 bit-for-bit in all five build modes + bench flat
(accessors are inline; codegen must not change measurably).

**Phase 2 — the 8B layout behind `LOVAX_NANBOX`:** same accessor API over a
`uint64_t`. BoxedIntObject + arithmetic overflow paths + NaN
canonicalization + GC pointer masking (`gcMarkValue`/barriers decode via
accessor). Gate: golden 83/83 bit-for-bit under NANBOX in all five modes
(the exact-int64 proof: goldens exercise 64-bit bit ops), fuzz, and an
interleaved 16B-vs-8B cross-bench table. 8B becomes default only if the
data pays; otherwise it waits (flag stays for the JIT era).

## Interactions

- **GC**: `gcMarkValue`/`gcShade` decode the pointer through the accessor;
  no raw `v.obj` remains anywhere. Write barrier logic unchanged.
- **Upvalues/coroutine stacks/globals**: all store Value — shrink 2× for
  free; coroutine gcBytes constant updates.
- **FFI (v1.0)**: the C ABI marshals through accessors, so it is layout-
  independent — this RFC MUST land before the ABI freezes.
- **JIT (v1.x)**: IR type guards test the 13+4 tag bits in one compare;
  number unboxing is a shift/mask. This layout is what the emitter assumes.
- **Interning (deferred from v0.17)**: revisit here — StringObject is
  already being touched; a cached hash field rides along cheaply.

## Rejected alternatives

- Pure LuaJIT/JS model (int ≤ 2^53 as double): breaks exact int64 — rejected.
- 16B forever: costs a permanent 2× value-traffic tax against the stated
  LuaJIT-class goal — rejected by decision (2026-07-19).
- Pointer tagging in low bits (V8 smi-style, 1-bit): halves int range per
  bit and forces double onto the heap — doubles are as common as ints in
  game code (positions, dt); rejected.

## Credits

Layout study: LuaJIT `lj_obj.h` (Mike Pall) — method studied, not copied;
V8 smi design notes; JavaScriptCore JSValue documentation. Consistent with
the project rule: learn from Lua/LuaJIT/CPython, implement our own.
