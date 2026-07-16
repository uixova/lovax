#!/usr/bin/env bash
# Timing harness for v0.8 performance work. Runs each stress program 3x and
# reports the best wall-clock time. Not a correctness test (see run_tests.sh).
set -u
cd "$(dirname "$0")/.."
LOVAX=./lovax
[ -x "$LOVAX" ] || { echo "build first: g++ -std=c++17 -O3 -fno-gcse -fno-crossjumping -o lovax src/main.cpp"; exit 2; }

run() {
    local name="$1" file="$2" best=99999
    for i in 1 2 3; do
        local s e ms
        s=$(date +%s.%N); "$LOVAX" "$file" >/dev/null 2>&1; e=$(date +%s.%N)
        ms=$(awk -v a="$s" -v b="$e" 'BEGIN{printf "%.0f",(b-a)*1000}')
        [ "$ms" -lt "$best" ] && best=$ms
    done
    printf "  %-16s %6s ms\n" "$name" "$best"
}

echo "Lovax benchmark (best of 3):"
run "heavy_loop"  examples/stress/heavy_loop.lov
run "roguelike"   examples/stress/roguelike.lov
run "pathfinding" examples/stress/pathfinding.lov
run "particles"   examples/stress/particles.lov
run "economy"     examples/stress/economy.lov
