#!/usr/bin/env bash
# Cross-language benchmark harness.
# Runs the same workload (identical output, verified) in Lovax, Lua 5.4, LuaJIT,
# Python 3, and Node, on THIS machine with the interpreters found on PATH.
# Metric = external wall-clock (best of N), so startup is included and every
# language is measured the exact same way. Also reports startup and peak RSS.
#
# Usage: benchmarks/cross/run.sh [reps]   (default 5)
set -u
cd "$(dirname "$0")/../.."
REPS="${1:-5}"
P="benchmarks/cross/progs"
LOVAX="./lovax"
[ -x "$LOVAX" ] || { echo "build first: g++ -std=c++17 -O3 -fno-gcse -fno-crossjumping -o lovax src/main.cpp"; exit 2; }

# lang key -> runner command; skipped automatically if the runner is missing.
LANGS=(lovax lua54 lua55 luajit python node)
declare -A CMD=(
  [lovax]="$LOVAX"
  [lua54]="lua5.4"
  [lua55]="lua5.5"
  [luajit]="luajit"
  [python]="python3"
  [node]="node"
)
declare -A EXT=(
  [lovax]="lov" [lua54]="lua" [lua55]="lua" [luajit]="lua" [python]="py" [node]="js"
)
BENCHES=(fib strcat hashmap btree gc regex jsonb)

have() { command -v "$1" >/dev/null 2>&1; }
# best-of-REPS wall-clock in ms for: runner file
best_ms() {
  local runner="$1" file="$2" best=99999999 s e ms
  for _ in $(seq 1 "$REPS"); do
    s=$(date +%s.%N); "$runner" $file >/dev/null 2>&1; e=$(date +%s.%N)
    ms=$(awk -v a="$s" -v b="$e" 'BEGIN{printf "%.1f",(b-a)*1000}')
    awk -v m="$ms" -v b="$best" 'BEGIN{exit !(m<b)}' && best=$ms
  done
  echo "$best"
}
peak_rss_mb() { # runner file  -> peak child RSS in MB (portable, via getrusage)
  python3 - "$1" "$2" <<'PY' 2>/dev/null || echo "?"
import sys, subprocess, resource
subprocess.run(sys.argv[1:], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
kb = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
print(f"{kb/1024:.1f}")
PY
}

# Resolve which languages are actually present.
active=()
for L in "${LANGS[@]}"; do
  c="${CMD[$L]}"; { [ "$L" = lovax ] && [ -x "$LOVAX" ]; } || have "$c" && active+=("$L")
done

echo "# Lovax cross-language benchmark — $(date +%Y-%m-%d), best of $REPS (ms, lower=better)"
echo "# host: $(uname -sm), g++ $(g++ -dumpversion 2>/dev/null)"
echo
# header
printf "%-10s" "bench"
for L in "${active[@]}"; do printf "%12s" "$L"; done
echo
printf -- "%-10s" "----------"
for L in "${active[@]}"; do printf "%12s" "-----------"; done
echo

for b in "${BENCHES[@]}"; do
  printf "%-10s" "$b"
  for L in "${active[@]}"; do
    f="$P/$b.${EXT[$L]}"
    if [ -f "$f" ]; then printf "%12s" "$(best_ms "${CMD[$L]}" "$f")"
    else printf "%12s" "n/a"; fi
  done
  echo
done

# startup (empty program)
printf "%-10s" "startup"
for L in "${active[@]}"; do
  f="$P/empty.${EXT[$L]}"
  [ -f "$f" ] && printf "%12s" "$(best_ms "${CMD[$L]}" "$f")" || printf "%12s" "n/a"
done
echo

# peak memory (MB) per workload
echo
echo "# peak RSS (MB), lower=better:"
for b in gc btree hashmap; do
  printf "%-10s" "mem:$b"
  for L in "${active[@]}"; do
    f="$P/$b.${EXT[$L]}"
    [ -f "$f" ] && printf "%12s" "$(peak_rss_mb "${CMD[$L]}" "$f")" || printf "%12s" "n/a"
  done
  echo
done

echo
