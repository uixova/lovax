#!/usr/bin/env bash
# Real multi-client network test: one Lovax TCP server accepts and answers
# THREE sequential client connections (separate lovax processes). Closes the
# quality-backlog gap ("no real multi-client test").
set -u
cd "$(dirname "$0")/.."
LOVAX=./lovax
[ -x "$LOVAX" ] || { echo "build first"; exit 2; }
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
PORT=$(( (RANDOM % 20000) + 30000 ))

cat > "$tmp/server.lov" <<LOV
use net
set srv = net.tcp_listen($PORT)
set served = 0
repeat 3:
    set c = net.tcp_accept(srv)
    set istek = net.recv(c)
    net.send(c, "yanit-" + istek)
    net.close(c)
    served += 1
say "served: {served}"
LOV
cat > "$tmp/client.lov" <<LOV
use net
use os
set c = net.tcp_connect("127.0.0.1", $PORT)
net.send(c, os.args()[0])
say net.recv(c)
net.close(c)
LOV

timeout 20 "$LOVAX" "$tmp/server.lov" > "$tmp/server.out" 2>&1 &
SRV=$!
sleep 0.3
fails=0
for i in 1 2 3; do
    out=$("$LOVAX" "$tmp/client.lov" "istemci$i" 2>&1)
    [ "$out" = "yanit-istemci$i" ] && echo "  ok: client $i -> $out" \
        || { echo "  FAIL: client $i got '$out'"; fails=$((fails+1)); }
done
wait $SRV
grep -q "served: 3" "$tmp/server.out" && echo "  ok: server served 3 clients" \
    || { echo "  FAIL: server output: $(cat "$tmp/server.out")"; fails=$((fails+1)); }
echo "net_multi: $fails failure(s)"
[ "$fails" -eq 0 ] && echo "NET MULTI-CLIENT GATE PASSED" || exit 1
