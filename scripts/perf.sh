#!/usr/bin/env bash
# Cross-check of docs/profiling.md with a sampling profiler. Needs Linux or WSL2 with g++ and perf.
# Usage: scripts/perf.sh [feed file]   (default: generates a 2M-message feed in a temp dir)
# cpu-clock is a software timer, so it works where hardware counters are unavailable (WSL2).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# Normal optimised build (inlining on); -g only adds symbols.
g++ -std=c++20 -O2 -g -fno-omit-frame-pointer -pthread -I"$ROOT/src" "$ROOT/src/main.cpp" -o "$WORK/engine"
if [ $# -ge 1 ]; then FEED=$(realpath "$1"); else FEED="$WORK/feed.bin"; "$WORK/engine" gen --n 2000000 --seed 1 --out "$FEED" >/dev/null; fi

cd "$WORK"
for book in map ladder; do
  echo "===== $book ====="
  perf record -F 999 -e cpu-clock -o "perf.$book" ./engine profile "$book" "$FEED" 40 >/dev/null 2>&1
  perf report -i "perf.$book" --no-children --stdio 2>/dev/null | grep -v '^#' | grep -v '^$' | head -14 | cut -c1-120
done
