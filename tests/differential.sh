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
$CXX -o "$TMP/nanbox" src/main.cpp                          # default: 8B NaN-box, computed-goto, JIT on
$CXX -DLOVAX_NO_NANBOX -o "$TMP/box16" src/main.cpp         # 16B tagged union (no JIT there)
$CXX -DLOVAX_NO_COMPUTED_GOTO -o "$TMP/nocg" src/main.cpp   # switch dispatch

run() { out=$("$1" "${@:2}" 2>&1); printf '%s|%s' "$?" "$out"; }
fail=0; n=0

diffcheck() { # file
    n=$((n+1))
    local a b c j r
    a=$(run "$TMP/nanbox" "$1")               # JIT on (default: RA compiler)
    b=$(run "$TMP/box16"  "$1")
    c=$(run "$TMP/nocg"   "$1")
    j=$(run "$TMP/nanbox" --no-jit "$1")      # same binary, JIT off — the key axis
    r=$(run "$TMP/nanbox" --no-ra "$1")       # template compiler (RA off) — the other JIT tier
    t=$(run "$TMP/nanbox" --no-trace "$1")    # trace on-by-default vs off — the Stage-5 axis
    nf=$(run "$TMP/nanbox" --no-numfn "$1")   # numfn on-by-default vs off — the Stage-6a axis
    if [ "$a" != "$b" ]; then
        echo "DIVERGENCE 8B vs 16B: $1"; diff <(printf '%s' "$a") <(printf '%s' "$b") | head -12; fail=1
    fi
    if [ "$a" != "$c" ]; then
        echo "DIVERGENCE CG vs NOCG: $1"; diff <(printf '%s' "$a") <(printf '%s' "$c") | head -12; fail=1
    fi
    if [ "$a" != "$j" ]; then
        echo "DIVERGENCE JIT-on vs JIT-off: $1"; diff <(printf '%s' "$a") <(printf '%s' "$j") | head -12; fail=1
    fi
    if [ "$a" != "$r" ]; then
        echo "DIVERGENCE RA-JIT vs template-JIT: $1"; diff <(printf '%s' "$a") <(printf '%s' "$r") | head -12; fail=1
    fi
    if [ "$a" != "$t" ]; then
        echo "DIVERGENCE default(trace-on) vs --no-trace: $1"; diff <(printf '%s' "$a") <(printf '%s' "$t") | head -12; fail=1
    fi
    if [ "$a" != "$nf" ]; then
        echo "DIVERGENCE default(numfn-on) vs --no-numfn: $1"; diff <(printf '%s' "$a") <(printf '%s' "$nf") | head -12; fail=1
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

echo "== JIT-targeted int while-loops (must equal interpreter exactly) =="
genloop() { # index -> an all-integer while loop the baseline JIT WILL compile
    local k=$1
    cat <<EOF
fn run(n):
    set acc = 0
    set i = 0
    while i < n:
        acc = acc + (i * $k + 7) % 13
        acc = acc & 1048575
        i = i + 1
    return acc
say run(200000)
set b = 0
set j = 0
while j < 100000:
    b = b + j - $k
    j = j + 1
say b
EOF
}
for k in 1 3 7 99 1234; do
    genloop "$k" > "$TMP/l.lov"
    diffcheck "$TMP/l.lov"
done

echo
echo "checked $n programs across 3 builds"
if [ "$fail" = 0 ]; then echo "DIFFERENTIAL GATE PASSED (all builds observationally identical)";
else echo "DIFFERENTIAL GATE FAILED"; exit 1; fi
