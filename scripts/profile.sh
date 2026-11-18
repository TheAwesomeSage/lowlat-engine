#!/usr/bin/env bash
# Profile the two order books with gprof (an instrumenting profiler). Needs Linux or WSL with g++ and gprof.
# Usage: scripts/profile.sh [feed file]   (default data/feed.bin; generate it first with `engine gen`)
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
FEED=$(realpath "${1:-$ROOT/data/feed.bin}")
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# -fno-inline-functions keeps the book's own functions visible as separate rows.
# -no-pie matters: with a position-independent binary gprof mislabels many symbols.
g++ -std=c++20 -O2 -fno-inline-functions -no-pie -fno-pie -pg -pthread -I"$ROOT/src" "$ROOT/src/main.cpp" -o "$WORK/engine_pg"

cd "$WORK"  # gprof writes gmon.out into the current directory
for book in map ladder; do
  ./engine_pg profile "$book" "$FEED" 40
  gprof -b -p ./engine_pg gmon.out | sed -n '1,14p' | cut -c1-110
  echo
done
