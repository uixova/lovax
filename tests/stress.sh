#!/usr/bin/env bash
# Load / stability gate — a sustained game frame loop that mixes every hot shape
# the JIT takes (array-of-structs entity update, struct-of-arrays float update
# with memory-base demotion, per-frame allocation churn, math intrinsics) while
# the incremental GC runs underneath. Asserts:
#   1. bit-identical output across every JIT tier and the interpreter (the oracle),
#   2. deterministic + leak-free across repeated runs (stable output, bounded RSS),
#   3. no crash (a signal exit >=128 fails).
# This is the repeatable "under load" check; run it after any JIT/GC change.
set -u
cd "$(dirname "$0")/.."
LOVAX="${LOVAX:-./lovax}"
[ -x "$LOVAX" ] || { echo "build first: make"; exit 2; }
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
prog="$tmp/game_load.lov"

cat > "$prog" <<'LOV'
struct Mob:
    x = 0.0
    y = 0.0
    vx = 0.0
    vy = 0.0
    hp = 0

set mobs = []
set px = []
set py = []
set pvx = []
set pvy = []
set i = 0
while i < 1500:
    push(mobs, Mob(i * 1.0, i * 0.5, 1.25, -0.75, 100))
    push(px, i * 1.0)
    push(py, 0.0)
    push(pvx, 1.5)
    push(pvy, -0.5)
    i = i + 1

set checksum = 0.0
set frame = 0
while frame < 400:
    set a = 0
    while a < 1500:
        set m = mobs[a]
        m.x = m.x + m.vx
        m.y = m.y + m.vy
        m.hp = m.hp - 1
        if m.hp < 0:
            m.hp = 100
        a = a + 1
    set b = 0
    while b < 1500:
        px[b] = px[b] + pvx[b]
        py[b] = py[b] + pvy[b]
        b = b + 1
    set temp = []
    set c = 0
    while c < 150:
        push(temp, sqrt(c * 1.0) + floor(c * 0.5) - abs(0 - c))
        c = c + 1
    set d = 0
    while d < 150:
        checksum = checksum + temp[d]
        d = d + 1
    frame = frame + 1

set s = 0
while s < 1500:
    set m = mobs[s]
    checksum = checksum + m.x + m.y + (m.hp * 1.0) + px[s] + py[s]
    s = s + 1
say floor(checksum)
LOV

fail=0

# 1) bit-identical across tiers (interpreter is the oracle)
oracle="$("$LOVAX" --no-jit "$prog" 2>&1)"
echo "  oracle (interpreter): $oracle"
for flags in "" "--no-ra" "--no-trace" "--no-numfn"; do
    got="$("$LOVAX" $flags "$prog" 2>&1)"
    code=$?
    label="${flags:-full-jit}"
    if [ "$code" -ge 128 ]; then echo "  CRASH ($code): $label"; fail=1
    elif [ "$got" != "$oracle" ]; then echo "  FAIL: $label gave '$got' != '$oracle'"; fail=1
    else echo "  ok: $label"; fi
done

# 2) deterministic + bounded memory across repeats (no drift, no leak)
prev=""
for r in 1 2 3 4 5; do
    got="$("$LOVAX" "$prog" 2>&1)"
    [ -n "$prev" ] && [ "$got" != "$prev" ] && { echo "  FAIL: run $r drifted ('$got' != '$prev')"; fail=1; }
    prev="$got"
done
[ "$fail" = 0 ] && echo "  ok: 5 repeats deterministic ($prev)"

echo "stress: $fail failure(s)"
[ "$fail" -eq 0 ] && echo "LOAD / STABILITY GATE PASSED" || exit 1
