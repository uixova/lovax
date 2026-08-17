#!/usr/bin/env bash
# Single-binary bundling gate (RFC-027). `lovax bundle app.lov -o app` must
# produce a self-contained executable: runs with no separate Lovax install, its
# argv reach the app as os.args(), and re-bundling stays size-stable.
set -u
cd "$(dirname "$0")/.."
LOVAX="${LOVAX:-./lovax}"
[ -x "$LOVAX" ] || { echo "build first: make"; exit 2; }
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
fail=0

cat > "$tmp/app.lov" <<'LOV'
use os
set total = 0
for i in range(1000):
    total = total + (i // 2)
say "bundled ok total={total} args={os.args()}"
LOV

"$LOVAX" bundle "$tmp/app.lov" -o "$tmp/app" >/dev/null 2>&1 || { echo "  FAIL: bundle command"; exit 1; }
[ -x "$tmp/app" ] || { echo "  FAIL: output not executable"; exit 1; }

# Run from a scratch dir with a PATH that has no lovax at all.
out=$(cd "$tmp" && PATH=/usr/bin:/bin ./app alpha beta 2>&1)
if echo "$out" | grep -q 'bundled ok total=249500 args=\["alpha", "beta"\]'; then
    echo "  ok: bundled app self-contained + args passed"
else
    echo "  FAIL: bundled output: $out"; fail=1
fi

# Bundling is deterministic: the same script twice yields the same size.
"$LOVAX" bundle "$tmp/app.lov" -o "$tmp/app_b" >/dev/null 2>&1
[ "$(stat -c%s "$tmp/app")" = "$(stat -c%s "$tmp/app_b")" ] \
    && echo "  ok: bundle is deterministic (size-stable)" \
    || { echo "  FAIL: bundle size not deterministic"; fail=1; }

# The plain interpreter (no trailer) must still run scripts normally.
echo 'say 6 // 4' > "$tmp/plain.lov"
[ "$("$LOVAX" "$tmp/plain.lov")" = "1" ] && echo "  ok: plain interpreter unaffected" || { echo "  FAIL: plain run"; fail=1; }

echo "bundle: $fail failure(s)"
[ "$fail" -eq 0 ] && echo "BUNDLE GATE PASSED" || exit 1
