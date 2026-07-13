#!/usr/bin/env bash
# Security test — the capability sandbox (RFC-015) actually denies.
# A permission feature is only real if it blocks when it should AND allows when
# granted. This asserts both directions for net / file-write / file-read / env.
set -u
cd "$(dirname "$0")/.."
LUME=./lume
[ -x "$LUME" ] || { echo "build first: g++ -std=c++17 -O3 -fno-gcse -fno-crossjumping -o lume src/main.cpp"; exit 2; }

tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
fails=0

# expect_deny <label> <flags...> -- <script>
# expect_ok   <label> <flags...> -- <script>
mk() { printf '%s\n' "$2" > "$tmp/$1.lm"; }

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

echo "sandbox denials:"
check deny "net (sandboxed)"        "$($LUME --sandbox "$tmp/net.lm" 2>&1)"
check deny "file write (sandboxed)" "$($LUME --sandbox "$tmp/fw.lm" 2>&1)"
check deny "env (sandboxed)"        "$($LUME --sandbox "$tmp/env.lm" 2>&1)"
check deny "write when only --allow-read" "$($LUME --allow-read "$tmp/fw.lm" 2>&1)"

echo "grants:"
check ok "net with --allow-net"     "$($LUME --sandbox --allow-net "$tmp/net.lm" 2>&1)"
check ok "write with --allow-write" "$($LUME --sandbox --allow-write "$tmp/fw.lm" 2>&1)"
check ok "env with --allow-env"     "$($LUME --sandbox --allow-env "$tmp/env.lm" 2>&1)"
check ok "all with --allow-all"     "$($LUME --sandbox --allow-all "$tmp/net.lm" 2>&1)"
check ok "default (no flags)"       "$($LUME "$tmp/net.lm" 2>&1)"

echo "sandbox: $fails failure(s)"
[ "$fails" -eq 0 ] && echo "SANDBOX GATE PASSED" || { echo "SANDBOX GATE FAILED"; exit 1; }
