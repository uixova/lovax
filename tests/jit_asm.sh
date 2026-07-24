#!/usr/bin/env bash
# JIT assembler gate (RFC-026 Stage 1). Two independent proofs of the encoder,
# run BEFORE any JIT logic depends on it:
#   1. behavioural — build real machine code, make it executable, call it,
#      check the answers (includes the exact NaN-box tag guard the JIT emits);
#   2. encoding — disassemble a canonical byte sequence with objdump and match
#      mnemonics, so a wrong encoding that happens to give the right answer
#      still fails.

set -u
cd "$(dirname "$0")/.."
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail=0

echo "== behavioural (generate, execute, verify) =="
if g++ -std=c++17 -O2 -o "$TMP/asm_tests" tests/jit/asm_tests.cpp 2>"$TMP/build.err"; then
    if "$TMP/asm_tests"; then :; else fail=1; fi
else
    echo "  FAIL: build"; cat "$TMP/build.err"; fail=1
fi

echo
echo "== encoding (objdump cross-check) =="
if ! command -v objdump >/dev/null 2>&1; then
    echo "  SKIP: objdump not installed (behavioural gate still ran)"
else
    "$TMP/asm_tests" --dump "$TMP/canon.bin"
    dis=$(objdump -D -b binary -m i386:x86-64 -M att "$TMP/canon.bin" 2>/dev/null)
    want=(
        "mov    %rdi,%rax"
        "add    %rsi,%rax"
        "sub    %rdx,%rax"
        "imul   %rcx,%rax"
        "and    %r9,%r8"
        "xor    %r11,%r10"
        "shl    \$0x11,%rax"
        "sar    \$0x11,%rax"
        "mov    (%rsp),%rbx"
        "mov    %rbx,-0x8(%rbp)"
        "movabs \$0x1122334455667788,%rdx"
        "neg    %rax"
        "not    %rax"
        "cqto"
        "idiv   %rcx"
        "push   %r12"
        "pop    %r12"
        "ret"
    )
    for w in "${want[@]}"; do
        if printf '%s' "$dis" | grep -qF "$w"; then
            echo "  ok: $w"
        else
            echo "  FAIL: expected '$w' in disassembly"; fail=1
        fi
    done
    if [ "$fail" != 0 ]; then echo "--- full disassembly ---"; printf '%s\n' "$dis"; fi
fi

echo
echo "== baseline code generator (isolated) =="
if g++ -std=c++17 -I. -O2 -o "$TMP/codegen_tests" tests/jit/codegen_tests.cpp 2>"$TMP/cg.err"; then
    if "$TMP/codegen_tests"; then :; else fail=1; fi
else
    echo "  FAIL: codegen test build"; cat "$TMP/cg.err"; fail=1
fi

echo
if [ "$fail" = 0 ]; then echo "JIT GATE PASSED"; else echo "JIT GATE FAILED"; exit 1; fi
