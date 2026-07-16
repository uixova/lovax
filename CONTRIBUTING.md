# Contributing to Lovax

Thanks for your interest! The process is simple.

## Development Setup

```bash
# Build (any C++17 compiler, zero dependencies)
g++ -std=c++17 -O3 -fno-gcse -fno-crossjumping -o lovax src/main.cpp

# Run the test suite (golden-file tests, both dispatch modes)
./tests/run_tests.sh

# Sanitizer build
g++ -std=c++17 -g -fsanitize=address,undefined -o lovax_asan src/main.cpp

# Security gate (adversarial + fuzz — must report no crashes)
./tests/fuzz.sh
```

## Release Gate — every step ends here (no exceptions)

A change is not "done" until **all** of these pass. From v0.10 on, the VM grows
memory-unsafe machinery (NaN-boxed values, a GC, and eventually a JIT), so this
gate is the contract that keeps the language crash-free and exploit-free:

1. **Golden, both dispatch modes** — `./tests/run_tests.sh` bit-for-bit under
   computed-goto **and** `-DLOVAX_NO_COMPUTED_GOTO`.
2. **Sanitizers clean** — ASan + UBSan + LeakSanitizer over every `tests/cases/*.lov`
   and every `examples/stress/*.lov`, in both dispatch modes for anything touching
   the VM core.
3. **Security gate** — `./tests/fuzz.sh` (run it against the ASan build too):
   adversarial + random inputs must **never** crash. A signal exit (≥128) is a
   hard failure; only Lovax's own exit codes (0/64/65/70) are acceptable.
4. **Perf non-regression** — `./tests/bench.sh`; a step must not silently make the
   language slower.
5. **A safety fallback exists** for any unsafe optimization — e.g. NaN-boxing ships
   behind `LOVAX_SAFE_VALUES`, and CI builds/tests both representations, so a bug in
   the fast path can never brick the language.

CI (`.github/workflows/ci.yml`) enforces 1–3 on every push and PR.

## Rules

1. **No change without a test.** New feature → add a `.lov` + `.expected` pair under
   `tests/cases/`. Verify the output by hand, then generate with `./tests/run_tests.sh --update`.
2. **Error messages are located, helpful, and in English.** Include the line number and,
   where possible, the fix (e.g. `"define it with: set x = ..."`).
3. **Language-changing decisions need an RFC first.** Add a short design doc under `rfcs/`:
   motivation → design → alternatives. Not needed for small fixes.
4. **Naming philosophy:** short, friendly, Lovax-flavored but never confusing
   (the `say` / `ask` / `text` / `kind` / `check` line).
5. **The core stays dependency-free.** No third-party libraries.
6. **Comments are written in English**, describing the code as it is — no changelog-style
   markers ("newly added", version tags) inside comments.

## Test File Format

`tests/cases/example.lov` is executed; its combined stdout+stderr and exit code are
compared against `example.expected`. Intentional-error tests are prefixed with `e`.
Note: many test scripts deliberately use Turkish identifiers — that exercises Lovax's
UTF-8 support and must keep working.
