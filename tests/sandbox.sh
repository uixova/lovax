#!/usr/bin/env bash
# Security test — the capability sandbox (RFC-015) actually denies.
# A permission feature is only real if it blocks when it should AND allows when
# granted. This asserts both directions for net / file-write / file-read / env.
set -u
cd "$(dirname "$0")/.."
LOVAX=./lovax
[ -x "$LOVAX" ] || { echo "build first: g++ -std=c++17 -O3 -fno-gcse -fno-crossjumping -o lovax src/main.cpp"; exit 2; }

tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
fails=0

# expect_deny <label> <flags...> -- <script>
# expect_ok   <label> <flags...> -- <script>
mk() { printf '%s\n' "$2" > "$tmp/$1.lov"; }

check() { # mode(deny|ok), label, output
    local mode="$1" label="$2" out="$3"
    if [ "$mode" = deny ]; then
        echo "$out" | grep -q "permission denied" && echo "  ok: $label denied" || { echo "  FAIL: $label was NOT denied"; fails=$((fails+1)); }
    else
        echo "$out" | grep -q "permission denied" && { echo "  FAIL: $label wrongly denied"; fails=$((fails+1)); } || echo "  ok: $label allowed"
    fi
}

mk net 'use net
say "opened"'
mk fw  "use file
file.write_text(\"$tmp/w.txt\", \"x\")
say \"wrote\""
mk fr  "use file
file.write_text(\"$tmp/r.txt\", \"x\")
say file.read_text(\"$tmp/r.txt\")"
mk env 'use os
say os.env("HOME")'
mk run 'use os
say os.run("echo x")'

echo "sandbox denials:"
check deny "net (sandboxed)"        "$($LOVAX --sandbox "$tmp/net.lov" 2>&1)"
check deny "file write (sandboxed)" "$($LOVAX --sandbox "$tmp/fw.lov" 2>&1)"
check deny "env (sandboxed)"        "$($LOVAX --sandbox "$tmp/env.lov" 2>&1)"
check deny "write when only --allow-read" "$($LOVAX --allow-read "$tmp/fw.lov" 2>&1)"
check deny "run (sandboxed)"        "$($LOVAX --sandbox "$tmp/run.lov" 2>&1)"

echo "grants:"
check ok "net with --allow-net"     "$($LOVAX --sandbox --allow-net "$tmp/net.lov" 2>&1)"
check ok "write with --allow-write" "$($LOVAX --sandbox --allow-write "$tmp/fw.lov" 2>&1)"
check ok "env with --allow-env"     "$($LOVAX --sandbox --allow-env "$tmp/env.lov" 2>&1)"
check ok "run with --allow-run"     "$($LOVAX --sandbox --allow-run "$tmp/run.lov" 2>&1)"
check ok "all with --allow-all"     "$($LOVAX --sandbox --allow-all "$tmp/net.lov" 2>&1)"
check ok "default (no flags)"       "$($LOVAX "$tmp/net.lov" 2>&1)"

echo "sandbox: $fails failure(s)"
[ "$fails" -eq 0 ] && echo "SANDBOX GATE PASSED" || { echo "SANDBOX GATE FAILED"; exit 1; }
