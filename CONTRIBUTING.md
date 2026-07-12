# Contributing to Lume

Thanks for your interest! The process is simple.

## Development Setup

```bash
# Build (any C++17 compiler, zero dependencies)
g++ -std=c++17 -O3 -fno-gcse -fno-crossjumping -o lume src/main.cpp

# Run the test suite (53 golden-file tests)
./tests/run_tests.sh

# Sanitizer build
g++ -std=c++17 -g -fsanitize=address,undefined -o lume_asan src/main.cpp
```

## Rules

1. **No change without a test.** New feature → add a `.lm` + `.expected` pair under
   `tests/cases/`. Verify the output by hand, then generate with `./tests/run_tests.sh --update`.
2. **Error messages are located, helpful, and in English.** Include the line number and,
   where possible, the fix (e.g. `"define it with: set x = ..."`).
3. **Language-changing decisions need an RFC first.** Add a short design doc under `rfcs/`:
   motivation → design → alternatives. Not needed for small fixes.
4. **Naming philosophy:** short, friendly, Lume-flavored but never confusing
   (the `say` / `ask` / `text` / `kind` / `check` line).
5. **The core stays dependency-free.** No third-party libraries.
6. **Comments are written in English**, describing the code as it is — no changelog-style
   markers ("newly added", version tags) inside comments.

## Test File Format

`tests/cases/example.lm` is executed; its combined stdout+stderr and exit code are
compared against `example.expected`. Intentional-error tests are prefixed with `e`.
Note: many test scripts deliberately use Turkish identifiers — that exercises Lume's
UTF-8 support and must keep working.
