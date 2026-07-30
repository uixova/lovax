# Cross-language benchmark

## Stage-5 trace compiler is now the DEFAULT (after RA) — 2026-07-28

The tracer graduated: `jitTraceEnabled = true`. The tier chain is now
**RA → trace → template → interpreter**. The RA keeps the pure-int-local loops it
is optimal at (intloop is unchanged: 187ms default, same as `--no-trace`); the
trace compiler takes the float / global numeric loops the RA declines. So the
float-kernel win is now out of the box, no flag:

| mandel (200×200×60) | default (was template) | **default (now trace)** | lua5.4 | luajit |
|---|---:|---:|---:|---:|
| time (ms) | 24 | **13** | 40 | 9 |

`--no-trace` restores the RA/template pipeline. Verified on the new default:
golden 108/0; differential 122 programs (default trace-on == `--no-trace` == 16B ==
switch == interpreter); robustness; jit_asm; ASan+UBSan+GC_STRESS_INC bit-identical.
Placing trace AFTER the RA means it can never regress a loop the RA already handled
and, being at least as good as the template on the float/global loops it takes, is
a pure win. (Full trace-tier-first + trace-linking is Stage-5.6e.)

## Stage-5.6d: floats live in XMM registers across the region — 2026-07-28

The tracer now has a real two-file register allocator: INT values stay UNBOXED in
GP registers and FLOAT values stay as DOUBLES in XMM registers for the whole
region. Stage-5.6a carried floats as their raw word in a GP register and bounced
each one into an XMM scratch for every SSE op (movq in / op / movq out); a float
threading through a chain of ops paid that bounce every step. Now a float op reads
its XMM operands directly (`addsd xmm,xmm`) and leaves the result in place — only a
genuine int→float coercion (`cvtsi2sd`) or the memory boundary (a plain `movsd` of
the 8-byte double, since a float's NaN-box word *is* the double) touches both
files. This is the same trick LuaJIT gets from keeping floats in xmm.

| mandel (200×200×60) | interp | default (template) | 5.6a trace | **5.6d trace** | lua5.4 | luajit |
|---|---:|---:|---:|---:|---:|---:|
| time (ms) | 116 | 24 | 14 | **12.8** | 40 | 9 |

The float kernel is now **1.85× the template default and within 1.4× of LuaJIT**
(from ~5× behind at Stage-4). Bit-identical across golden (108/0), the differential
`--jit-trace` axis (122 programs), a 500-program numeric-loop fuzz, a 300-program
call-inline fuzz, and ASan+UBSan+GC_STRESS_INC. `--jit-trace` stays opt-in.

## Stage-5.6c: call inlining into loop traces — 2026-07-28

The trace compiler now inlines a called function's body into a hot loop, so the
call boundary (trampoline round-trip, frame push/pop, dispatch) vanishes. A global
whose GET feeds a CALL is recognised as a callee (never a numeric cell); the leaf
callee — a params-only, single-`return`, branch/call-free numeric expression — has
its bytecode spliced into the trace with its params mapped to the argument
registers already on the operand stack. The callee global is guarded against
reassignment (its exact closure word is compared at the call site); a mismatch, or
any guard inside the inlined body, side-exits to the interpreter *at the CALL* with
the pre-call stack intact (the inline's temps sit above it and are never written).

| loopcall (20M, 2 helpers/iter) | interp | default (template) | **--jit-trace** |
|---|---:|---:|---:|
| time (ms) | 4964 | 4818 | **4424** |

~9% over the template default here (two trampoline calls per iteration removed);
the win scales with call density. Bit-identical across golden (108/0, incl. new
call-inline torture 82 — int/float/multi-arg helpers cross-checked, plus a
mid-loop callee swap that exercises the reassignment guard), the differential
`--jit-trace` axis (122 programs), a 300-program call-inline fuzz, and
ASan+UBSan+GC_STRESS_INC.

**Honest scope:** this inlines calls *inside loops*. It does NOT touch pure tree
recursion (fib) — fib has no loop for the region tracer to trigger on, so it needs
the record-while-execute trace recorder (follow the call across frames, unroll
down-recursion, trace-link up-recursion) — the larger next slice. Call inlining is
the reusable call-boundary machinery that recorder builds on.

## Stage-5.6a/b: the tracing JIT lands — float + globals — 2026-07-28

The first FLOAT-capable region compiler (`src/jit/trace_record.hpp`, gated behind
`--jit-trace`). It is the register allocator's numeric successor: floats travel
as their raw NaN-box word in a GP register and enter an XMM scratch only for the
SSE op; globals are pinned to registers (read-only ones need no writeback —
Stage-5.6b's dirty-tracking snapshot); every cell's type is observed from the
live value the moment the loop turns hot and guarded in the prologue (runtime
type recording, so float type-inference is free). Guards side-exit to the
interpreter with a compact deopt snapshot.

| mandel (200×200×60) | interp | default (template) | **--jit-trace** | lua5.4 | luajit | node |
|---|---:|---:|---:|---:|---:|---:|
| time (ms) | 116 | 41 | **24** | 40 | 9 | 62 |

So the trace compiler is **1.7x the template default, ~4.8x the interpreter**, and
now edges plain Lua (40) on this float kernel; LuaJIT (9) stays ahead — the gap is
the per-region-entry prologue guards that fire on all ~40k pixels, which Stage-5.6d
(LICM / guard-hoisting) removes. Bit-identical to the interpreter across golden
(107/0, incl. self-verifying float + guard/deopt torture 80–81), the differential
`--jit-trace` axis (119 programs), a 400-program numeric-loop fuzz, and
ASan+UBSan+GC_STRESS_INC. `--jit-trace` is opt-in until the tracer matures through
5.6c (recursion inline) → 5.6d (optimizer) → 5.6e (linking), then it becomes the
default trace tier ahead of RA.

## Stage-5 start: the register allocator is now the DEFAULT — 2026-07-28

The RA proved itself (bit-identical to the interpreter across all 105 goldens, the
differential RA axis, and the incremental-GC gate), so it is on by default now;
`--no-ra` falls back to the template compiler. Consequence for the table below:
`intloop` at the default build is now the RA number, not the template number —

| intloop (30M) | default (RA) | --no-ra (template) | lua5.4 | luajit | node |
|---|---:|---:|---:|---:|---:|
| time (ms) | **356** | 702 | 866 | 452 | 186 |

so **out of the box Lovax now beats plain Lua (2.4x) and LuaJIT (1.3x) on integer
compute** — only V8/Node ahead. The other cross-benches are unchanged (the RA
falls back on their float/recursion hot code); those are the tracing JIT's job
(Stage-5 core). The historical sections below say "opt-in behind --jit-ra" —
that described the state at the time; it is the default from here on.

## Stage-4 cross-language snapshot — 2026-07-28 (best of 3, ms, lower=better)

Full field on this host (g++ 16), default `./lovax` (template JIT + the Stage-4c
call fast path; the RA is opt-in and does not fire on these workloads — see
below). Output verified identical across languages.

| bench   | lovax | lua5.4 | lua5.5 | luajit | python | node |
|---------|------:|-------:|-------:|-------:|-------:|-----:|
| fib     | 250   | 203    | 207    | **30** | 330    | 72   |
| intloop | 678†  | 866    | 452    | 4037   | **186**| —    |
| strcat  | 139   | **21** | 28     | 33     | 45     | 69   |
| hashmap | **171** | 419  | 411    | 159    | 244    | 444  |
| btree   | 205   | 210    | **177**| 89     | 126    | 120  |
| gc      | 67    | 57     | 55     | **5**  | 88     | 63   |
| regex   | **18**| n/a    | n/a    | n/a    | 68     | 71   |
| jsonb   | **58**| n/a    | n/a    | n/a    | 95     | 92   |
| mandel  | 47    | 40     | 43     | **9**  | 404    | 62   |
| sieve   | 219   | 181    | 144    | **47** | 861    | 90   |
| qsort   | 280   | 251    | 237    | **104**| 651    | 126  |
| startup | 6.4   | 4.2    | 3.4    | 3.6    | 24     | 45   |

Peak RSS (MB): gc 19 · btree 35 · hashmap 56 — in the Lua tier, well under Node.

† `intloop` is a pure-integer compute loop (30M iters) — the RA's target. The
table shows the DEFAULT build (template JIT). With the RA on (`--jit-ra`) it is a
different story:

| intloop (30M) | interp | template | **RA** | lua5.4 | luajit | node |
|---|---:|---:|---:|---:|---:|---:|
| time (ms) | 2333 | 678 | **342** | 866 | 452 | 186 |

**With the register allocator, Lovax on integer compute beats plain Lua (2.5x)
AND LuaJIT (1.3x)** — only V8/Node is ahead. LuaJIT keeps integers as doubles
(5.1 semantics), so a non-power-of-two modulo is a float divide; Lovax keeps
exact int64 and emits an integer `idiv`. This is the one place Stage-4 already
overtakes LuaJIT, and it is exactly the workload the RA was built for. (The RA is
opt-in until it is made the default after more soak.)

**Where Lovax leads the non-JIT field:** hashmap (own open-addressing map beats
both Luas, Python, Node — only LuaJIT ahead), regex (own engine, 3-4x over
Python/Node), jsonb, startup. **Where it trails:** the compute/call benches
(fib, mandel, sieve, qsort) sit ~1.2-1.5x behind plain Lua and ~5-10x behind
LuaJIT; strcat is O(n^2) copy.

### What Stage-4's RA actually did — and where it did not

The register allocator's win is real but *targeted*. On a pure-integer compute
loop it is decisive:

| pure-int hot loop (30M) | interp | template | RA (`--jit-ra`) |
|---|---:|---:|---:|
| time | 2176 | 628 | **251** (2.5x over template) |

But it does **not** move the cross-language table, and this is the honest,
important finding for Stage-5:

- **qsort**: default 279 ms == `--jit-ra` 279 ms. Indexing is guard-bound (each
  access re-checks is-object / is-list / bounds), and at -O3 the template
  compiler is already good, so the RA breaks even. The win needs **LICM** to
  hoist the loop-invariant list guards.
- **mandel**: float loop — the RA is integer-only so far; it falls back to the
  template (47 ms). Needs **unboxed-float RA** (Stage-4b-float), targeted at the
  ~2.5x the integer path got.
- **fib**: call-bound — RA doesn't apply; Stage-4c's fast path gave ~6%. The real
  win is **tracing** (Stage-5).
- **sieve**: bool-keyed list — needs float/bool index + LICM.

So Stage-4 delivered the register-allocation *foundation* and a proven 2.5x on
the workload it targets, but the cross-language movers are the next three:
**unboxed-float RA (mandel), LICM guard-hoist (qsort/sieve), tracing (fib)** —
which is exactly the Stage-5 priority order this snapshot sets.

## Stage-4c: compiled-call fast path — 2026-07-27

A compiled body's CALL goes through a C++ trampoline. Stage-4c gives it a fast
path: a non-variadic closure called with exactly its parameters and holding a
compiled body has its frame pushed directly, skipping callValue's generic
validation (variadic collection, arg-count error construction, builtin
dispatch); anything else falls to callValue unchanged. fib(32): 305 -> 288 ms
(~6%), same result, golden 105/0, differential 119 (all builds identical).

**Honest ceiling:** this is small on purpose. fib is bottlenecked by the
per-call compiled-body invocation itself — prologue/epilogue, dispatch, ctx
setup — which no method-JIT frame trick removes; only TRACING (inlining the
recursion into one flat trace) does. So the real call/recursion win is Stage-5,
and Stage-4c takes only the safe, low-risk slice. A machine-code frame setup was
considered and rejected: ~1.3x for a high corruption risk (ClosureObject ->
shared_ptr -> Proto -> Chunk offset chain), not worth it before tracing.

## Stage-4b: list indexing in the register allocator — 2026-07-27

Stage-4a's RA was integer-only. Stage-4b adds list indexing so array kernels
(qsort-style) compile there too. The register model gains a small type state per
value — VT_INT (unboxed int64 in a GP reg) and VT_BOXED (a raw NaN-box word, for
a list base or a freshly-loaded element) — and a value is guarded+unboxed to
VT_INT only when it feeds an integer op. A pre-pass classifies each local by an
origin scan: a local used as an index BASE is a list (kept as the raw word),
every other local is an int.

INDEX_GET/SET emit the object guard, the LIST tag guard, the vector data+size
load, an unsigned bounds check (which catches negative indices too) and the
element load/store. GC safety: a region is CALL-free so it never allocates, but
the incremental collector may be mid-MARK on entry, so an INDEX_SET whose value
carries a heap pointer BAILS to the interpreter (which stores it with the write
barrier); storing an int/float/bool needs no barrier and stays in machine code —
the array-of-numbers hot path.

| bench (index-bound) | template JIT | RA JIT | 
|---|---:|---:|
| qsort (300k) | 331 ms | 293 ms |
| sorting torture (5 algos) | 88 ms | 95 ms |

**Honest:** the index win is small (qsort ~11%, pure-index break-even) because
indexing is GUARD-bound, not box/unbox-bound — every access re-checks is-object /
is-list / bounds. The list type is loop-invariant, so hoisting those guards out
of the loop (LICM) is what turns this into a real win; that is the Stage-4
optimizer pass. What lands here is the correct foundation: array loops now run in
the register allocator, bit-identical to the interpreter across all goldens, the
differential gate (RA axis) and the incremental-GC write-barrier gate.

## Stage-4a: register-allocating JIT — 2026-07-27

The template JIT keeps the operand stack in memory and re-boxes every
intermediate: each integer op is guardInt + unbox + unbox + compute + box
(~25 instructions). The new register-allocating region compiler (compile_ra.hpp,
RFC-027 Stage-4a) keeps integers UNBOXED in registers across the whole region:
a value is guarded + unboxed ONCE on entry (locals in the prologue, constants at
compile time), every op works on raw int64s, and a result only gets a cheap
RANGE CHECK (still fits the 47-bit inline payload?) before continuing. Boxing
happens solely at the memory boundary — normal exit and bail — and never
overflows because every result was range-checked. Per-op cost ~25 → ~6
instructions.

Depth-indexed operand registers (position d is always pool[numLocals+d]) make
every branch/merge automatically consistent, so internal jumps need no flush;
bail boxes the live locals + operands back and returns the resume offset, the
same interpreter-takeover contract as the template compiler (no deopt). Scope:
CALL-free integer loops that fit the 7-register pool; anything else falls back
to the template compiler, which stays the differential oracle.

Hot all-integer loop (`acc += (i*3+7)%13; acc &= 0xFFFFFF`, 30M iterations):

| | time | vs interp |
|---|---:|---:|
| interpreter (`--no-jit`) | 2466 ms | 1.0× |
| template JIT (default)   | 621 ms  | 4.0× |
| **RA JIT** (`--jit-ra`)  | **253 ms** | **9.7×** |

**2.45× over the template JIT**, same result. Opt-in behind `--jit-ra` until it
is the default — zero regression risk. Correctness: RA output is bit-identical
to the interpreter across all 105 goldens and to the template JIT in the
differential gate (new axis), ASan-clean on the JIT torture suite.

**Honest scope:** the win lands on pure-integer, call-free loops (compute
kernels). Mixed loops move less — heavy_loop 244→232 ms — because their hot
regions use list indexing / floats / calls Stage-4a does not take yet. Those are
Stage-4b (unboxed float + list index) and 4c (compiled-to-compiled calls, for
fib). This is the register-allocation foundation the rest of Stage-4 builds on.

## JIT Stage-3: compiled function bodies + call trampoline — fib 1.18× — 2026-07-27

Recursive / call-bound code (fib) has no loop, so the loop-triggered JIT never
fired on it. Stage-3 adds **whole-function** compiled bodies: a function that has
been called `JIT_BODY_THRESHOLD` times gets its whole body compiled (start→
RETURN), cached on the Chunk (`jitBodyFn` — a pointer check in the CALL fast
path, no per-call hash lookup). RETURN hands back to the interpreter at the
RETURN so it does the frame pop; a `CALL` inside a compiled body goes through a
**trampoline** (`g_jitTrampoline`) that runs the call through the interpreter's
own machinery — which re-enters compiled bodies, so recursion stays compiled.
Also added: the fused immediate compares `LT_I_JF … NE_I_JF` (fib's `if n < 2`).

This is sound because the value stack is a fixed array (never reallocated) and
`frames_` is reserved to MAX_FRAMES=500, so the base pointers the compiled code
holds stay valid across a nested call, and the trampoline's C++ recursion is
bounded (~500 deep, well under the 8 MB stack; deeper recursion raises the normal
"max call depth" error, not a crash). The trampoline syncs `ctx->sp ↔ vm->sp_`
around the call so the GC sees every live slot.

**A measured lesson (why two extra tricks were needed):** naive trampolining was
*slower* than the interpreter on fib (224 vs 197 ms) — the per-call round-trip
(callValue + a fresh `run()` + a hash lookup) cost more than the compiled body
saved. Two fixes flipped it: (1) cache the body pointer on the Chunk, no hash
lookup; (2) execute the common `RETURN` inline in the trampoline instead of
spinning up a full `run()`. That is the crux LuaJIT sidesteps entirely by
**tracing** — it inlines the recursion into a straight-line trace, with no
per-call boundary. We studied LuaJIT's call/recursion recording (`lj_record.c`
`rec_call*`/`rec_func*`, down-recursion unroll + up-recursion trace-linking) to
choose this: full tracing is a much larger architecture, so a baseline
method-JIT with a trampoline is the sound, tractable step here (study, not copy;
credited).

JIT-on vs JIT-off, same binary, best-of-5:

| bench | JIT off | JIT on | speedup |
|-------|--------:|-------:|--------:|
| fib   | 199 | **168** | **1.18×** |

Modest next to LuaJIT's tracing (which does fib several× faster), but a real
win, and `btree` (call/struct-heavy) is unchanged — the CALL fast-path body
check adds no measurable interpreter overhead. Correctness: golden 100/100
bit-identical JIT-on (new case 74: recursion, mutual recursion, a builtin called
from a compiled body, error propagation through the trampoline, and an int64
overflow mid-body), differential 111 programs, GC_STRESS+ASan and the
incremental-barrier gate clean, fuzz 0 crashes.

**A true fib win to LuaJIT's level needs Stage-4:** compiled-to-compiled direct
calls (machine-code frame setup, no interpreter round-trip) or a tracing JIT.

## JIT Stage-2 expansion: index / if / for — 2026-07-26

The baseline JIT now compiles list indexing (`INDEX_GET`/`INDEX_SET`),
conditionals (`JUMP_IF_FALSE`), `for … in range()` (`FOR_NEXT`, range fast
path), the `nil`/`true`/`false` literals and top-level `set` (`DEFINE_GLOBAL`).
Index access is where the unboxed storage (previous section) pays off in machine
code: a bounds check then a direct 8-byte `Value` load/store, no boxing.
`INDEX_SET` only stores an immediate (int/float/bool/nil) in compiled code — a
value carrying a pointer bails to the interpreter, so no write barrier is ever
needed inside the region. Non-list objects, maps/strings, negative or
out-of-range indices, and non-range iterators all bail cleanly.

JIT-on vs JIT-off, same binary, same session, best-of-5 (the controlled A/B for
the JIT itself):

| bench | JIT off | JIT on | speedup |
|-------|--------:|-------:|--------:|
| sieve | 498 | **233** | **2.1×** |
| qsort | 582 | **286** | **2.0×** |
| mandel | 135 | **45** | **3.0×** |

`mandel` lands via the float path added next: `ADD`/`SUB`/`MUL` and the fused
comparisons (`LESS_JF` … `GREATER_EQ_JF`) now take an int fast path, then a
float path (SSE2 — the Value bytes ARE the double: `movsd` load, `addsd`/
`subsd`/`mulsd`, `ucomisd` compare), else bail. A mixed int/float pair bails
(the interpreter promotes); a NaN result or unordered compare bails (the
interpreter does the write-time NaN canonicalisation). New encoder ops
`subsd`/`mulsd`/`ucomisd` + P/NP conditions, each with a behavioural test.

Correctness held throughout: golden 99/99 bit-identical with JIT on,
differential 111 programs (JIT-on == JIT-off == 16-byte == switch),
GC_STRESS+ASan and the incremental-barrier gate clean, a plain-ASan JIT-active
sweep clean, fuzz 0 crashes, the assembler/codegen unit gate green. New golden
cases 72 (index/for/if/define-global) and 73 (float add/sub/mul + float compare)
run in hot loops.

**Still not JIT'd:** calls (fib) — that is Stage 3 (cross-function frames).

## Unboxed list storage — Lovax before/after (2026-07-25)

`ListObject::elements` changed from `std::vector<Ref<Object>>` (one heap object
per element) to `std::vector<Value>` (the 8-byte NaN-boxed value stored inline).
An int/float element is now the inline word; the VM's INDEX_GET/SET, list build,
iteration and unpack read/write the `Value` directly — no per-element boxing.
`Value` was hoisted into object.hpp (ahead of the containers) to make this
possible; builtins convert at their boundary (`toObject`/`fromObject`), the hot
VM opcodes do not.

Same machine/day, best-of-4, external wall-clock (startup included), old =
git HEAD before the change, new = this change. JIT for/index NOT active yet
(that is Stage-2 expansion) — this is a **pure interpreter** win:

| bench   | old (boxed) | new (unboxed) | change |
|---------|------:|------:|--------|
| qsort   | 936   | **343** | **−63% (2.7×)** |
| sieve   | 315   | 313   | flat |
| btree   | 114   | 118   | flat |
| hashmap | 106   | 108   | flat |

**Honest reading:** the win lands exactly where elements are **numbers**. qsort
sorts an int array — every swap/partition write used to allocate a fresh
IntegerObject (`toObject(int)`); unboxing deletes millions of allocations →
2.7×. sieve is **flat because it stores `true`/`false`**, and booleans were
already shared singletons (TRUE_OBJ/FALSE_OBJ) — no per-element allocation
existed to remove. btree/hashmap don't use list indexing (structs/maps), so
flat as expected. All outputs verified bit-identical to the boxed build and to
the 16-byte fallback; golden 97/97, GC_STRESS+ASan and incremental-barrier
gates clean. Next: Stage-2 JIT (for/index/if/float) turns index access into a
direct machine-code load on this unboxed storage — that is where sieve moves.

## JIT is LIVE (RFC-026 Stage 2) — 2026-07-25

The baseline JIT is now wired into the VM: a hot loop (≥50 back-edges) is
compiled to x86-64 and run in machine code. Default on where it is compiled in
(NaN-box + x86-64); `--no-jit` disables it, `--jit-stats` reports activity.

Correctness contract, proven: golden 97/97 with JIT ON is bit-identical to the
interpreter, and the differential harness runs every program JIT-on vs JIT-off
(110 programs) with identical output.

Measured on an all-integer `while` loop (1,000,000 iterations):

| | time |
|---|---:|
| interpreter (`--no-jit`) | 67 ms |
| **JIT** | **22 ms** |

**3.0× end-to-end** (includes startup + the interpreted warm-up before the
threshold), same result.

**Honest scope:** this Stage-2 JIT compiles integer `while`-loops (locals,
globals, int arithmetic, comparisons, jumps). It does NOT yet handle `for`
loops (iterator opcodes), array indexing, `if/else` branches, floats or calls —
those regions fall back to the interpreter cleanly (a float loop bails and is
blacklisted; an unsupported opcode simply is never compiled). So the
cross-language table below — whose workloads use `for`, indexing and floats —
is UNCHANGED by the JIT for now. Closing that gap is Stage 2 expansion
(if/index/for) and Stage 3 (calls, for fib), plus the separately-identified
unboxed-list-storage work.

## v1.0.1 — 10 workloads, 6 runtimes (2026-07-24)

Best of 5, external wall-clock (startup included), same machine. **Every
runner's output is verified against Lovax's before its time is reported** —
a runner that errors out exits instantly and would otherwise look like the
fastest language in the table (a LuaJIT syntax error once "won" qsort at
3.3 ms). Three new, harder workloads this round: `mandel` (float compute
loop), `sieve` (integer + list indexing, 2M), `qsort` (hand-written
quicksort — recursion + array indexing + swaps, 300k).

**JIT is NOT active in these numbers.** The code generator exists and is
proven (2.7× on a hot loop in isolation) but is not yet wired into the VM.
This is the honest pre-JIT baseline.

| bench   | lovax | lua 5.4 | lua 5.5 | luajit | python | node |
|---------|------:|--------:|--------:|-------:|-------:|-----:|
| fib     | 313   | 192     | 195     | **30** | 302    | 74   |
| strcat  | 99    | **22**  | 24      | 29     | 27     | 49   |
| hashmap | 133   | 291     | 288     | **95** | 175    | 371  |
| btree   | 189   | 182     | 162     | **85** | 115    | 76   |
| gc      | 53    | 46      | 42      | **4**  | 103    | 73   |
| regex   | **15**| n/a     | n/a     | n/a    | 52     | 53   |
| jsonb   | **63**| n/a     | n/a     | n/a    | 93     | 83   |
| mandel  | 128   | 42      | 46      | **8**  | 351    | 64   |
| sieve   | 438   | 154     | 126     | **59** | 810    | 82   |
| qsort   | 1215  | 227     | 227     | 98     | 631    | **121** |
| startup | 5.1   | 3.7     | 4.0     | 3.7    | 24     | 59   |

Peak RSS (MB): gc 15.3 (tied best) · btree 34 (Lua 24, LuaJIT 40, Node 71) ·
hashmap 56 (Lua 31, LuaJIT 19, Node 113).

### Nerede iyiyiz
- **hashmap 133** — Lua 5.4/5.5 (291/288), Python (175) ve Node (371) hepsini
  geçiyor; sadece LuaJIT önde. v0.17'nin open-addressing + cached-hash index'i
  işini yapıyor.
- **regex 15** — Python `re` ve Node regex'inden **3.5× hızlı** (kendi motorumuz).
- **jsonb 63** — Python (93) ve Node'u (83) geçiyor.
- **startup 5.1 ms** — Python 24, Node 59. Lua ile aynı ligde.
- **Bellek** gc'de en iyilerle eşit.

### Nerede geriyiz — ve fark ne kadar
| bench | Lua'ya göre | Neden |
|---|---|---|
| qsort | **5.4× geri** | dizi indeks get/set + recursion |
| sieve | 3.5× geri | dizi indeks, sıkı döngü |
| mandel | 3.0× geri | float aritmetik döngüsü |
| fib | 1.6× geri | çağrı maliyeti |
| strcat | 4.6× geri | `s = s + x` O(n²) kopya (Lua'da da öyle ama bizde per-op yük fazla) |

Python'u mandel'de 2.7×, sieve'de 1.8× geçiyoruz; Node/LuaJIT gibi JIT'li
runtime'larla saf compute farkı duruyor.

### Farkı kapatacak iki kaldıraç (ölçüm bunu söylüyor)

**1. JIT (devam ediyor).** Dispatch + aritmetik maliyetini siler. İzole
ölçümde sıcak int döngüsünde 2.7× alındı. mandel/sieve/fib doğrudan bunun
hedefi.

**2. Listelerde kutusuz depolama (yeni bulgu, JIT'ten bağımsız).**
`ListObject::elements` şu an `std::vector<Ref<Object>>` — yani listedeki her
tamsayı ayrı bir heap nesnesi. Her erişim bir pointer dolaylaması, 300k
elemanlı dizi 300k ayrı nesne demek. Lua array part'ta değerleri **inline**
tutuyor, farkın büyük kısmı burada. `std::vector<Value>` (8 byte, kutusuz)
yapmak qsort/sieve/btree'yi ve belleği doğrudan iyileştirir — map index'inde
(%46 kazanç) işe yarayan aynı sınıf kalıcı veri-yapısı düzeltmesi.

qsort'un neden en kötümüz olduğu (5.4×) bununla açıklanıyor: hem indeks
dolaylaması hem çağrı maliyeti aynı anda çarpıyor.


## v0.18 8-byte NaN-boxed Value — Lovax before/after (2026-07-19)

RFC-024 landed: value = one register, exact int64 kept via transparent
boxing. Interleaved best-of-7, same machine/day:

| bench   | v0.17 (16B) | v0.18 (8B) | change |
|---------|------:|------:|--------|
| fib     | 232   | **194** | −16% |
| btree   | 111   | **102** | −8%  |
| hashmap | 86    | 85    | flat |
| strcat  | 66    | 64    | flat |
| gc      | 31    | 31    | flat |

No regressions. Cumulative v0.16 → v0.18 (clean interleaved baselines):
fib 230→194 (−16%), hashmap 168→85 (−49%), gc 46→31 (−33%), btree 125→102
(−18%), heavy_loop 315→~150 (−52%) — all before any JIT. (The RESULTS.md
v0.14 table's fib 416 was measured under post-compile machine load; the
clean same-day baseline is 230 — recorded here for honesty.)

## v0.17 runtime acceleration — Lovax before/after (2026-07-19)

Same machine, same day, interleaved best-of-5 (only Lovax re-measured; the
other languages' columns below are from the v0.14 run on the same host —
re-run the full field with `run.sh` before quoting cross-language numbers).

| bench   | v0.16 | v0.17 | change | what did it |
|---------|------:|------:|--------|-------------|
| gc      | 46    | **28**  | −39% | pool allocator |
| hashmap | 168   | **91**  | −46% | pool + open-addressing cached-hash index |
| btree   | 125   | **109** | −13% | pool allocator |
| heavy_loop | 315 | **156** | −50% | both (maps + allocation everywhere) |
| fib     | 230   | 231   | flat | by design: compute gap is the JIT's job (v1.x) |
| strcat  | 62    | 64    | flat | copy-bound; in-place append needs escape analysis (Track B) |

Memory: hashmap peak RSS 66→56 MB, btree 37→34 MB, rest flat.

# v0.14 field table (2026-07-17)

Same machine, same workload (outputs verified identical across all languages),
external wall-clock best-of-5. Reproduce with `benchmarks/cross/run.sh`.

Host: Linux x86_64, g++ 16. Interpreters: Lovax 0.14.0, Lua 5.4, Lua 5.5,
LuaJIT, CPython 3.14.6, Node 26.4.

## Time (ms, lower = better)

| bench   | Lovax | Lua 5.4 | Lua 5.5 | LuaJIT | Python | Node |
|---------|------:|--------:|--------:|-------:|-------:|-----:|
| fib(32) | **416** | 202 | 206 | 41 | 346 | 78 |
| strcat  | **111** | 23  | 30  | 31 | 28  | 43 |
| hashmap | 278 | 296 | 286 | 104 | 196 | 388 |
| btree   | 216 | 199 | 175 | 85 | 106 | 80 |
| gc      | 79  | 41  | 38  | 4  | 90  | 59 |
| regex   | **14** | n/a | n/a | n/a | 44 | 49 |
| json    | 80  | n/a | n/a | n/a | 101 | 72 |
| startup | 6   | 4   | 4   | 4  | 21  | 46 |

**New in v0.14: the regex and json rows exist at all** — both were "missing
feature" gaps in v0.11. And they arrive winning: Lovax's own step-limited
regex engine is **3× faster than CPython's `re` and Node's regex** on this
workload (compiled+cached bytecode, no PCRE machinery), and `json`
parse+stringify beats CPython (80 vs 101 ms; Node 72).

## Peak memory (MB, lower = better)

| bench   | Lovax | Lua 5.4 | Lua 5.5 | LuaJIT | Python | Node |
|---------|------:|--------:|--------:|-------:|-------:|-----:|
| gc      | **15**  | 15  | 15 | 15 | 15 | 59 |
| btree   | 37  | 24  | 26 | 35 | 15 | 71 |
| hashmap | 66  | 31  | 31 | 19 | 37 | 113 |

## v0.11 → v0.12 (compact structs RFC-017 + honest GC accounting)

| metric | v0.11 | v0.12 | change |
|---|---:|---:|---|
| btree time | 653 ms | **216 ms** | 3.0× faster |
| btree peak | 270 MB | **37 MB** | 7.3× smaller — now in Lua's league |
| gc peak    | 30 MB  | **15 MB** | matches Lua/LuaJIT/Python exactly |
| everything else | — | — | unchanged (no regressions) |

Two fixes did this: (1) a struct instance is now a shape pointer + flat slot
array instead of a full map with three per-instance hash indexes and per-instance
method closures; (2) the GC threshold now tracks *live* bytes (payload-aware,
recomputed at sweep) instead of a never-decreasing allocation total — dead
garbage no longer piles up between ever-rarer collections. `lovax --mem-stats`
prints allocations / collections / peak / total GC time / max pause.

## Honest reading (no spin)

**Memory is fixed.** Lovax now sits in the Lua tier on every memory bench.

**Pure compute is still the gap** — fib/strcat are ~2× behind Lua 5.4 and the
whole field trails LuaJIT by ~10×. That is interpreter dispatch + call cost;
the fix is the planned v1.x JIT (LuaJIT-inspired, zero-dep), NOT interpreter
micro-tuning — that code gets replaced wholesale. btree/hashmap show the
runtime's data structures are already competitive (hashmap beats every
non-JIT language here).

**Startup stays a win** (5 ms; Python 20, Node 37).

**Missing benches are real language gaps:** no regex engine, no in-memory JSON
parse (`load_data` is file-only). Both land in v0.14 (own regex engine RFC-021,
`json` module) — then this harness grows regex + json rows.

The JIT (v1.x) remains the plan to close the compute gap. This table is the
measured "before" picture it will be judged against.

---

### Archived: v0.11 honest baseline (2026-07-16)

| bench   | Lovax 0.11 | Lua 5.4 | LuaJIT | Python | Node |
|---------|------:|--------:|-------:|-------:|-----:|
| fib(32) | 389 | 190 | 37 | 308 | 73 |
| strcat  | 102 | 19  | 28 | 26  | 45 |
| hashmap | 240 | 279 | 100 | 186 | 409 |
| btree   | 653 | 179 | 97 | 104 | 75 |
| gc      | 82  | 45  | 4  | 92  | 51 |
| mem: btree | **270 MB** | 24 | 31 | 15 | 70 |
| mem: gc    | 30 MB | 15 | 15 | 15 | 59 |

That table refuted the old RFC-013 claim ("beats CPython everywhere") and
triggered the v0.12 memory work above.
