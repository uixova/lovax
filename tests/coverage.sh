#!/usr/bin/env bash
# Line-coverage measurement: builds with --coverage, runs the golden suite,
# reports the covered-line percentage for src/. (gcov, no external tools.)
set -u
cd "$(dirname "$0")/.."
echo "building with --coverage (slow)..."
g++ -std=c++17 -O0 --coverage -o lovax_cov src/main.cpp || exit 2
rm -f lovax_cov-main.gcda
pass=0
for lm in tests/cases/*.lov; do
    name=$(basename "$lm" .lov)
    LOVAX_BIN=./lovax_cov
    ./lovax_cov "$lm" > /dev/null 2>&1
    pass=$((pass+1))
done
echo "ran $pass golden scripts"
gcov -n lovax_cov-main 2>/dev/null | awk '
    /^File/ { f=$2; gsub(/'"'"'/, "", f) }
    /^Lines executed/ {
        split($0, a, ":"); split(a[2], b, "% of ");
        pct = b[1]; total = b[2];
        covered = pct / 100 * total;
        if (f ~ /src\//) { c += covered; t += total }
    }
    END { if (t > 0) printf "src/ line coverage: %.1f%% (%d / %d lines)\n", c * 100 / t, c, t }'
rm -f lovax_cov lovax_cov-main.gcda lovax_cov-main.gcno
