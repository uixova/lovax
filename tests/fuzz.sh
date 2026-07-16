#!/usr/bin/env bash
# Security gate — run at the end of every step.
# Feeds adversarial + random inputs to lovax and asserts it NEVER crashes:
# a real crash is a signal exit (>=128, e.g. 139 SIGSEGV / 134 SIGABRT).
# Any Lovax-defined exit code (0 ok, 64 usage, 65 syntax, 70 runtime) is fine —
# those mean "handled cleanly". A crash means an exploitable/unsafe hole.
set -u
cd "$(dirname "$0")/.."
LOVAX=./lovax
[ -x "$LOVAX" ] || { echo "build first: g++ -std=c++17 -O3 -fno-gcse -fno-crossjumping -o lovax src/main.cpp"; exit 2; }

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
fails=0
runs=0

check() { # name, file
    runs=$((runs+1))
    "$LOVAX" "$2" >/dev/null 2>&1
    local code=$?
    if [ "$code" -ge 128 ]; then
        echo "CRASH ($code, signal $((code-128))): $1"
        fails=$((fails+1))
    fi
}

# --- 1) hand-written adversarial programs -------------------------------------
cat > "$tmp/a1.lov" <<'E'
fn f(n): return f(n) + f(n)
f(1)
E
cat > "$tmp/a2.lov" <<'E'
set s = ""
for i in 0..200000: s = s + "xyz-{i}-"
say len(s)
E
cat > "$tmp/a3.lov" <<'E'
struct N:
    v = 0
    next = null
fn build(k): return N(k, build(k-1)) if k > 0 else null
say build(100000)
E
cat > "$tmp/a4.lov" <<'E'
set c = spawn(fn(): while true: yield 1)
for i in 0..100000: resume(c)
say "done"
E
cat > "$tmp/a5.lov" <<'E'
set m = {}
for i in 0..100000: m["k{i}"] = [i, i*i, "s{i}"]
say len(m)
E
for f in "$tmp"/a*.lov; do check "$(basename "$f")" "$f"; done

# --- 2) structural fuzz: deep nesting, unbalanced, truncated ------------------
python3 - "$tmp" <<'PY'
import os, random, sys
d = sys.argv[1]
random.seed(1)
toks = ['set','fn','if','else','for','in','yield','spawn','resume','struct',
        'return','while','(',')','[',']','{','}',':','=','+','-','*','/','%',
        '.','?.','..','x','1','"a"','n','true','null','\n','    ','say','try',
        'catch','throw','..','->','and','or','==','<','>','yield','fib']
# deep nesting
open(f"{d}/f_deep.lov","w").write("say " + "("*9000 + "1" + ")"*9000 + "\n")
open(f"{d}/f_deepbr.lov","w").write("say " + "["*9000 + "]"*9000 + "\n")
open(f"{d}/f_deepidx.lov","w").write("set x=[1]\nsay x" + "[0]"*5000 + "\n")
# unbalanced / truncated
open(f"{d}/f_trunc.lov","w").write("fn f(:\n  set x = \n  if")
open(f"{d}/f_str.lov","w").write('say "' + 'a'*100000)          # unterminated string
open(f"{d}/f_interp.lov","w").write('say "' + '{'*5000 + '1')     # unterminated interp
# random token soup
for i in range(60):
    n = random.randint(5, 400)
    open(f"{d}/r{i}.lov","w").write(" ".join(random.choice(toks) for _ in range(n)))
PY
for f in "$tmp"/f_*.lov "$tmp"/r*.lov; do check "$(basename "$f")" "$f"; done

echo "fuzz: $runs inputs, $fails crash(es)"
[ "$fails" -eq 0 ] && echo "SECURITY GATE PASSED — no crashes." || { echo "SECURITY GATE FAILED"; exit 1; }
