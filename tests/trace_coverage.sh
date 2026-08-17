#!/usr/bin/env bash
# Trace-coverage gate — the guard against pattern-fragility. Each snippet is a
# COMMON way to write a hot loop; every one MUST reach the trace JIT (compiled >=
# 1), so that a small change in how a game writes its code never silently loses
# the speedup. When a new common op is added to the language, add a case here; if
# a case starts failing, an op fell out of trace coverage and must be restored.
# (Genuinely un-traceable shapes belong in the separate deferred-op plan, not here.)
set -u
cd "$(dirname "$0")/.."
LOVAX="${LOVAX:-./lovax}"
[ -x "$LOVAX" ] || { echo "build first: make"; exit 2; }
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
fail=0

# name | program (must contain a hot loop that traces)
check_traces() {
    local name="$1" prog="$2"
    printf '%s\n' "$prog" > "$tmp/p.lov"
    local compiled
    compiled=$("$LOVAX" --jit-stats "$tmp/p.lov" 2>&1 | grep -oE 'compiled: [0-9]+' | grep -oE '[0-9]+')
    if [ "${compiled:-0}" -ge 1 ]; then
        echo "  ok: $name (compiled $compiled)"
    else
        echo "  FAIL: $name did NOT trace (compiled ${compiled:-0}) — an op fell out of coverage"
        fail=1
    fi
}

H='set i = 0
while i < 5000:'
F='    i = i + 1'

check_traces "int arithmetic"        "set s=0
$H
    s = s + i * 3 - 1
$F
say s"
check_traces "float arithmetic"      "set s=0.0
$H
    s = s + (i * 1.5) - 0.5
$F
say floor(s)"
check_traces "true division"         "set s=0.0
$H
    s = s + (i / 7)
$F
say floor(s)"
check_traces "floor div / mod"       "set s=0
$H
    s = s + (i // 7) + (i % 3)
$F
say s"
check_traces "unary negate"          "set s=0
$H
    s = s + (-i)
$F
say s"
check_traces "fused local arith"     "fn r():
    set s=0
    set i=0
    while i < 5000:
        set a = i + 1
        s = s + a
        i = i + 1
    return s
say r()"
check_traces "list index read"       "set xs=[]
set k=0
while k<200:
    push(xs,k)
    k=k+1
set s=0
$H
    s = s + xs[i % 200]
$F
say s"
check_traces "list compound assign"  "set xs=[]
set k=0
while k<200:
    push(xs,0)
    k=k+1
set f=0
while f<200:
    set j=0
    while j<200:
        xs[j] += 1
        j=j+1
    f=f+1
say xs[0]"
check_traces "struct field update"   "struct E:
    x = 0.0
    hp = 0
set e=E(0.0,1000)
$H
    e.x += 0.5
    e.hp -= 1
$F
say floor(e.x)"
check_traces "list literal alloc"    "set s=0
$H
    set t = [i, i+1, i+2]
    s = s + t[0]
$F
say s"
check_traces "math intrinsics"       "set s=0.0
set i=1
while i<5000:
    s = s + sqrt(i*1.0) + floor(i*0.5) + abs(0.0 - i)
    i=i+1
say floor(s)"

check_traces "for-range loop"        "set s=0
for i in range(5000):
    s = s + i
say s"
check_traces "for-range list body"    "set s=0
for i in range(5000):
    set t = [i, i+1]
    s = s + t[0]
say s"

check_traces "push number build"     "set xs=[]
set i=0
while i<5000:
    push(xs, i*1.0)
    i=i+1
say len(xs)"

check_traces "struct construction"   "struct V:
    x = 0.0
    hp = 0
set s=0.0
set i=0
while i<5000:
    set p = V(i*1.0, i)
    s = s + p.x
    i=i+1
say floor(s)"

check_traces "spawn push struct"     "struct P:
    x = 0.0
set parts=[]
set i=0
while i<5000:
    push(parts, P(i*1.0))
    i=i+1
say len(parts)"

check_traces "tuple literal"         "set s=0
$H
    set t = (i, i+1)
    s = s + t[0]
$F
say s"

echo "trace_coverage: $fail failure(s)"
[ "$fail" -eq 0 ] && echo "TRACE COVERAGE GATE PASSED" || exit 1
