#!/usr/bin/env bash
# Differential test: the 8-byte NaN-boxed Value (default) and the 16-byte
# fallback MUST be observationally identical, and so must computed-goto vs the
# portable switch. Runs every golden case AND a batch of generated programs
# through each build and asserts byte-identical stdout+stderr+exit. A divergence
# is a value-representation or dispatch bug the fixed goldens might not pin.

set -u
cd "$(dirname "$0")/.."
CXX="g++ -std=c++17 -O2"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

echo "building variants..."
$CXX -o "$TMP/nanbox" src/main.cpp                          # default: 8B NaN-box, computed-goto
$CXX -DLOVAX_NO_NANBOX -o "$TMP/box16" src/main.cpp         # 16B tagged union
$CXX -DLOVAX_NO_COMPUTED_GOTO -o "$TMP/nocg" src/main.cpp   # switch dispatch

run() { out=$("$1" "$2" 2>&1); printf '%s|%s' "$?" "$out"; }
fail=0; n=0

diffcheck() { # file
    n=$((n+1))
    local a b c
    a=$(run "$TMP/nanbox" "$1")
    b=$(run "$TMP/box16"  "$1")
    c=$(run "$TMP/nocg"   "$1")
    if [ "$a" != "$b" ]; then
        echo "DIVERGENCE 8B vs 16B: $1"; diff <(printf '%s' "$a") <(printf '%s' "$b") | head -12; fail=1
    fi
    if [ "$a" != "$c" ]; then
        echo "DIVERGENCE CG vs NOCG: $1"; diff <(printf '%s' "$a") <(printf '%s' "$c") | head -12; fail=1
    fi
}

echo "== all golden cases across 3 builds =="
for f in tests/cases/*.lov; do diffcheck "$f"; done

echo "== generated programs (int64 edges, boxed ints, mixed arithmetic) =="
gen() { # index -> a program that exercises value-representation corners
    local i=$1
    cat <<EOF
set a = $((i * 1000003 + 7))
set big = 9007199254740993 + $i
set neg = -9223372036854775807 - 1 + $i
set m = {}
m[big] = a
m[neg] = a * 2
set xs = [big, a, neg, big * 2, a - big]
say sort(xs)
say max(xs)
say min(xs)
say sum([big, a, neg])
say big % 1000
say (a * a) - (a * a)
say big == 9007199254740993 + $i
say neg < 0
for k in range(3):
    set c = big + k
    say c
fn f(x):
    return x * 3 + big
say f(a)
EOF
}
for i in 0 1 2 3 17 99 1000 65535 1000000; do
    gen "$i" > "$TMP/g.lov"
    diffcheck "$TMP/g.lov"
done

echo
echo "checked $n programs across 3 builds"
if [ "$fail" = 0 ]; then echo "DIFFERENTIAL GATE PASSED (all builds observationally identical)";
else echo "DIFFERENTIAL GATE FAILED"; exit 1; fi
