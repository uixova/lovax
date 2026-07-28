#!/usr/bin/env bash
# Robustness gate: pathological source (huge flat expressions, deep expression
# nesting, deep left-associative chains, deep block nesting, huge literals) must
# produce a GRACEFUL error, never a crash. A segfault/abort is exit >=128; the
# gate fails on any of those. Normal-size versions of each must still run.
#
# These pin the parser's MAX_EXPR_DEPTH / MAX_EXPR_CHAIN / MAX_BLOCK_DEPTH guards
# (a deep left-nested AST otherwise stack-overflows the compiler + AST destructor).

set -u
cd "$(dirname "$0")/.."
LOVAX=./lovax
[ -x "$LOVAX" ] || { echo "build first"; exit 2; }
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
fail=0

# runs a program, fails the gate only on a crash (exit >= 128). A graceful
# syntax/runtime error (65/70) or success (0) both pass.
nocrash() { # label file
    timeout 30 "$LOVAX" "$2" >/dev/null 2>&1; local c=$?
    if [ "$c" -ge 128 ]; then echo "CRASH ($c): $1"; fail=1
    else echo "ok ($c): $1"; fi
}
runs() { # label file expected
    local out; out=$(timeout 30 "$LOVAX" "$2" 2>&1)
    if [ "$out" = "$3" ]; then echo "ok: $1"; else echo "WRONG ($out): $1"; fail=1; fi
}

py() { python3 -c "$1" > "$2"; }

# --- pathological: must not crash ---
py "print('say ' + '+'.join(['1']*200000))"                       "$TMP/flat.lov";   nocrash "huge flat expr (200k terms)"    "$TMP/flat.lov"
py "print('say ' + '('*20000 + '1' + ')'*20000)"                  "$TMP/nest.lov";   nocrash "deep paren nesting (20k)"       "$TMP/nest.lov"
py "print('set a=[0]'); print('say a' + '[0]'*100000)"            "$TMP/idx.lov";    nocrash "deep index chain (100k)"        "$TMP/idx.lov"
py "print('say a' + '.b'*100000)"                                 "$TMP/mem.lov";    nocrash "deep member chain (100k)"       "$TMP/mem.lov"
py "print('say ' + '['*100000 + ']'*100000)"                      "$TMP/list.lov";   nocrash "deep list nesting (100k)"       "$TMP/list.lov"
py "
import sys;L=[]
for i in range(20000): L.append(' '*i+'if true:')
L.append(' '*20000+'say 1');sys.stdout.write(chr(10).join(L))"    "$TMP/blk.lov";    nocrash "deep block nesting (20k)"       "$TMP/blk.lov"

# --- normal-size versions: must still work ---
py "print('say ' + '+'.join(['1']*9000))"                         "$TMP/ok1.lov";    runs "9000-term expr"  "$TMP/ok1.lov" "9000"
py "print('say len(\"' + 'a'*500000 + '\")')"                     "$TMP/ok2.lov";    runs "500k string"     "$TMP/ok2.lov" "500000"
py "print('say len([' + ','.join(['1']*30000) + '])')"            "$TMP/ok3.lov";    runs "30k-elem list"   "$TMP/ok3.lov" "30000"
py "
L=[]
for i in range(60): L.append(' '*i+'if true:')
L.append(' '*60+'say 42');print(chr(10).join(L))"                 "$TMP/ok4.lov";    runs "60-deep nesting" "$TMP/ok4.lov" "42"

echo
[ "$fail" = 0 ] && echo "ROBUSTNESS GATE PASSED" || { echo "ROBUSTNESS GATE FAILED"; exit 1; }
