#!/usr/bin/env bash
# Lovax golden-file test runner.
# Usage:  ./tests/run_tests.sh            (from the repo root)
#         ./tests/run_tests.sh --update   (regenerate expected outputs — verify diffs first!)
#
# Every tests/cases/*.lov file is executed; its combined stdout+stderr output is
# compared byte-for-byte against the .expected file of the same name.
# The exit code is also verified via the "# exit: N" line at the top of .expected.

set -u
cd "$(dirname "$0")/.."

LOVAX=./lovax
CASES=tests/cases
UPDATE=0
[ "${1:-}" = "--update" ] && UPDATE=1

if [ ! -x "$LOVAX" ]; then
    echo "HATA: '$LOVAX' bulunamadı. Önce derle: g++ -std=c++17 -O3 -fno-gcse -fno-crossjumping -o lovax src/main.cpp"
    exit 2
fi

mkdir -p tests/tmp
pass=0
fail=0
failed_names=()

for lm in "$CASES"/*.lov; do
    name=$(basename "$lm" .lov)
    expected_file="$CASES/$name.expected"

    actual=$("$LOVAX" "$lm" 2>&1)
    code=$?
    actual_full="# exit: $code
$actual"

    if [ "$UPDATE" = "1" ]; then
        printf '%s\n' "$actual_full" > "$expected_file"
        echo "GÜNCELLENDİ: $name (exit $code)"
        continue
    fi

    if [ ! -f "$expected_file" ]; then
        echo "EKSİK BEKLENEN: $name ($expected_file yok)"
        fail=$((fail+1))
        failed_names+=("$name")
        continue
    fi

    expected=$(cat "$expected_file")
    if [ "$actual_full" = "$expected" ]; then
        pass=$((pass+1))
    else
        fail=$((fail+1))
        failed_names+=("$name")
        echo "BAŞARISIZ: $name"
        echo "--- beklenen ---"
        printf '%s\n' "$expected" | head -20
        echo "--- gerçekleşen ---"
        printf '%s\n' "$actual_full" | head -20
        echo "----------------"
    fi
done

echo ""
echo "Sonuç: $pass geçti, $fail kaldı."
if [ "$fail" -gt 0 ]; then
    echo "Kalanlar: ${failed_names[*]}"
    exit 1
fi
exit 0
