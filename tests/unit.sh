#!/usr/bin/env bash
# C++ unit tests for VM internals. Part of the release gate.
set -u
cd "$(dirname "$0")/.."
g++ -std=c++17 -O1 -o tests/unit/unit_tests tests/unit/unit_tests.cpp || exit 2
./tests/unit/unit_tests
