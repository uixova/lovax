#!/usr/bin/env bash
# Embed / FFI bridge gate (RFC-025). Builds and runs the C++ and C embedding
# demos and checks their output, then verifies the --allow-ffi capability gate.
# Pure C/C++ stdlib — no dependencies (host_demo_c also links -lstdc++ -lm).

set -u
cd "$(dirname "$0")/.."
CXX="g++ -std=c++17 -O2"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail=0

check() { # name, output, needle
    if printf '%s' "$2" | grep -qF "$3"; then echo "  ok: $1"; else
        echo "  FAIL: $1 (missing: $3)"; echo "$2" | tail -5; fail=1; fi
}

echo "== embed: C++ host demo =="
if $CXX examples/embed/host_demo.cpp -o "$TMP/host_demo" 2>"$TMP/e1"; then
    out=$("$TMP/host_demo" examples/embed/game.lov); rc=$?
    check "C++ demo exit 0" "rc=$rc" "rc=0"
    check "host->script frames" "$out" "get_frames()=5"
    check "script->host player_x" "$out" "player_x()=80"
    check "owning finalizer freed bullets" "$out" "live entities after GC: 1"
    check "persistent survives GC" "$out" "persistent player handle valid: yes"
else echo "  FAIL: C++ demo build"; cat "$TMP/e1"; fail=1; fi

echo "== embed: C ABI demo =="
if $CXX -c src/embed/lovax.cpp -o "$TMP/capi.o" 2>"$TMP/e2" && \
   gcc -O2 examples/embed/host_demo_c.c "$TMP/capi.o" -lstdc++ -lm -o "$TMP/host_c" 2>"$TMP/e3"; then
    out=$("$TMP/host_c"); rc=$?
    check "C demo exit 0" "rc=$rc" "rc=0"
    check "C ABI native call (c_add)" "$out" "42"
    check "C ABI read global" "$out" "total (from script) = 15"
    check "C ABI host->script call" "$out" "double(21) = 42"
else echo "  FAIL: C ABI build"; cat "$TMP/e2" "$TMP/e3"; fail=1; fi

echo "== embed: --allow-ffi capability gate =="
cat > "$TMP/gate.cpp" <<'EOF'
#include <cstdio>
#include <cstring>
#include "../../src/embed/embed.hpp"
using namespace Lovax; using namespace Lovax::Embed;
int main(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "--sandbox") == 0) StdLib::perms().ffi = false;
    Host rt;
    rt.native("read_secret", [](const std::vector<Ref<Object>>& a, int line,
                                const BuiltinObject::CallFn&) -> Ref<Object> {
        if (!StdLib::perms().ffi)  // dangerous native self-gates (RFC-025)
            return makeError("permission denied: read_secret requires --allow-ffi", line);
        return makeObj<StringObject>("s3cr3t");
    });
    if (!rt.loadSource("say read_secret()\n"))
        std::printf("%s\n", rt.lastError()->inspect().c_str());
    return 0;
}
EOF
# heredoc path is relative to tests/, fix include base by compiling from repo root
sed -i 's#../../src/embed/embed.hpp#src/embed/embed.hpp#' "$TMP/gate.cpp"
if $CXX -I. "$TMP/gate.cpp" -o "$TMP/gate" 2>"$TMP/e4"; then
    denied=$("$TMP/gate" --sandbox 2>&1)
    allowed=$("$TMP/gate" 2>&1)
    check "gate denies under sandbox" "$denied" "permission denied"
    check "gate allows by default"    "$allowed" "s3cr3t"
else echo "  FAIL: gate build"; cat "$TMP/e4"; fail=1; fi

echo "== embed: re-entrancy (native -> script -> native) + error across boundary =="
cat > "$TMP/reentry.cpp" <<'EOF'
#include <cstdio>
#include "src/embed/embed.hpp"
using namespace Lovax; using namespace Lovax::Embed;
int main() {
    Host rt;
    // a native that calls BACK into a script function via the CallFn — the
    // deepest re-entrancy path (native -> script closure -> native ...).
    rt.native("apply_twice", [](const std::vector<Ref<Object>>& a, int line,
                                const BuiltinObject::CallFn& call) -> Ref<Object> {
        if (a.size() != 2) return makeError("apply_twice(fn, x)", line);
        auto once = call(a[0], { a[1] }, line);            // script fn
        if (isError(once)) return once;                     // propagate script error
        return call(a[0], { once }, line);                  // feed result back in
    });
    rt.native("boom", [](const std::vector<Ref<Object>>&, int line,
                         const BuiltinObject::CallFn&) -> Ref<Object> {
        return makeError("native boom", line);              // error crosses back to script
    });
    bool ok = rt.loadSource(
        "fn inc(x):\n    return x + 1\n"
        "say apply_twice(inc, 10)\n"                         // 10 -> 11 -> 12
        "fn dbl(x):\n    return x * 2\n"
        "say apply_twice(dbl, 9007199254740993)\n"          // boxed int through re-entrancy
        "set caught = false\n"
        "try:\n"
        "    boom()\n"
        "catch e:\n"
        "    caught = true\n"
        "    say \"script caught: {e}\"\n"
        "say caught\n");
    if (!ok) { std::printf("load failed: %s\n", rt.lastError()->inspect().c_str()); return 1; }
    return 0;
}
EOF
if $CXX -I. "$TMP/reentry.cpp" -o "$TMP/reentry" 2>"$TMP/e5"; then
    out=$("$TMP/reentry" 2>&1); rc=$?
    check "re-entrancy exit 0" "rc=$rc" "rc=0"
    check "native->script->native (inc)" "$out" "12"
    check "boxed int through re-entrancy" "$out" "36028797018963972"
    check "native error caught by script" "$out" "script caught:"
    check "script recovered after native error" "$out" "true"
else echo "  FAIL: reentry build"; cat "$TMP/e5"; fail=1; fi

echo
if [ "$fail" = 0 ]; then echo "EMBED GATE PASSED"; else echo "EMBED GATE FAILED"; exit 1; fi
