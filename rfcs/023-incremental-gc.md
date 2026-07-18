# RFC-023 — incremental GC: the ≤1 ms game-frame contract

## Problem

The v0.11 collector was stop-the-world: mark(live graph) + sweep(every object)
in one pause. Measured worst pause on the btree benchmark: **13.6 ms** — an
entire 60 FPS frame budget, unacceptable for the engine.

## Design (tri-color + Dijkstra barrier, Lua-family shape)

- `Object.gcColor` ∈ {WHITE, GRAY, BLACK}. A cycle = MARK slices → atomic
  finish → SWEEP slices, all driven from the existing safepoints
  (loop back-edges + calls). Budget: 3000 objects/slice.
- **Write barrier** (`gcShade`): while MARKing, any reference written into a
  pre-existing container shades the CHILD gray. Container color is not
  checked — over-shading is extra work, never a bug. Sites: the
  `MapObject::set/setStr` chokepoint (covers maps + Sets everywhere),
  VM INDEX_SET / MEMBER_SET (list, struct, map-IC), list `push`/`insert`,
  deque pushes, signal `connect`, `SET_UPVALUE` + `closeUpvalues`, and a
  coroutine that suspends while black re-grays itself.
- **Stacks are not barriered** (a barrier per VM push would tax everything):
  the atomic FINISH re-scans all roots and drains what that grays. Root sets
  are small — the finish stays inside the budget class.
- **Sweep-phase births** go on a `newborn` side list the cursor never touches,
  spliced back (still white) at cycle end. The first attempt allocated them
  BLACK in place; stale black objects then leaked into the next cycle and
  their children were swept while reachable — caught as a **wrong btree
  checksum (524044 ≠ 524280)** before release. The side list removes the
  hazard structurally.

## Verification (two stress modes)

- `LOVAX_GC_STRESS`: full STW cycle at EVERY allocation (root completeness —
  a missed root is an immediate ASan use-after-free). 81/81 golden clean.
- `LOVAX_GC_STRESS_INC` (new): **budget = 1** — one-object slices, maximal
  mutator/collector interleaving, the barrier-hole torture test. 81/81 golden
  **bit-for-bit** + ASan/UBSan/Leak clean, including the btree checksum.

## Measured results (same machine)

| workload | v0.15 max pause | v0.16 max pause |
|---|---:|---:|
| btree | 13.6 ms | **0.18 ms** (74×) |
| gc bench | ~4.3 ms | **0.19 ms** |
| particles (game frame) | — | 0 collections in-run |
| hashmap | ~9 ms | 4.9 ms* |

\* One 100k-entry map is scanned atomically when the marker reaches it —
granularity is per-object, exactly like Lua's incremental mode. A resumable
container scan is the known v2 refinement if giant single tables ever matter
for games. Times and peaks elsewhere unchanged (btree 37 MB, bench flat).
