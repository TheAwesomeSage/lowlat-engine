#!/usr/bin/env bash
# Build the engine and the tests with g++ (MSYS2 UCRT64 on Windows, or any g++ >= 10 on Linux).
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build data results
CXX=${CXX:-g++}
FLAGS="-std=c++20 -O2 -Wall -Wextra -pthread"
$CXX $FLAGS -Isrc src/main.cpp -o build/engine
$CXX $FLAGS -Isrc tests/tests.cpp -o build/tests
echo "built build/engine and build/tests"
