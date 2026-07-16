# RFC-005: The Game Standard Library

**Status:** ACCEPTED (implemented)

## Motivation
Every game project hand-writes the same tools: lerp/clamp, Penner easing, weighted
random (loot tables), an event system, save/load. GDScript ships some of these;
Unity C# relies on third-party packages. Lovax ships them IN the language — zero setup.

## Scope
1. **math**: lerp, clamp, remap, sign, wrap, move_toward, dist, snap, trig (sin/cos/tan/
   asin/acos/atan2), exp/log, deg/rad + PI, TAU.
2. **Easing**: ease(t, name) — Penner set (linear, in/out/in_out × quad/cubic/sine +
   out_back, out_elastic, out_bounce). Engine tweens will build on this.
3. **Random**: pick(list), pick_weighted(map), shuffle(list) — all deterministic via seed(n).
4. **Events**: signal() / connect / disconnect / emit — event-driven code before the engine
   exists. Builtins may call Lovax functions through the CallFn bridge.
5. **Timers**: timer(seconds) / timer_done / timer_left / timer_reset — poll-based cooldowns.
6. **Persistence**: save_data/load_data (JSON), text/lines/bytes/CSV file IO.
7. **Higher-order**: each, filter, transform, sort_by.

## Naming principle
Short, friendly, Lovax-flavored, never confusing: the say/ask/text/num/kind line.
`transform`, not `map` (avoids clashing with the map type); `pick`, not `choice`.

## Deferred
- Tween/Timer *objects* driven by a frame loop → v1.0 (engine).
- Coroutines/await → after the VM (needs VM-owned call frames).
- Vector2/Color types → with struct (RFC-003).
