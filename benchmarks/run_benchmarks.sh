#!/usr/bin/env bash
# Lume benchmark runner — appends results to history.csv.
# Usage: ./benchmarks/run_benchmarks.sh [label]
set -u
cd "$(dirname "$0")/.."
LABEL="${1:-$(date +%Y-%m-%d)}"
VERSION=$(./lume --version | awk '{print $2}')
echo "Lume $VERSION — benchmark ($LABEL)"
for b in benchmarks/*.lm; do
    name=$(basename "$b" .lm)
    ms=$(./lume "$b" | grep "süre:" | grep -oE '[0-9]+')
    echo "$name: ${ms} ms"
    echo "$LABEL,$VERSION,$name,$ms" >> benchmarks/history.csv
done
