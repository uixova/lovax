#!/usr/bin/env bash
# Lovax benchmark runner — appends results to history.csv.
# Usage: ./benchmarks/run_benchmarks.sh [label]
set -u
cd "$(dirname "$0")/.."
LABEL="${1:-$(date +%Y-%m-%d)}"
VERSION=$(./lovax --version | awk '{print $2}')
echo "Lovax $VERSION — benchmark ($LABEL)"
for b in benchmarks/*.lov; do
    name=$(basename "$b" .lov)
    ms=$(./lovax "$b" | grep "süre:" | grep -oE '[0-9]+')
    echo "$name: ${ms} ms"
    echo "$LABEL,$VERSION,$name,$ms" >> benchmarks/history.csv
done
