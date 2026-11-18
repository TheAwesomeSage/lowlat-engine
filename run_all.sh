#!/usr/bin/env bash
# One command reproduces every number in the README: build, test, generate the feed, run every stage.
# Output of each stage is saved in results/. The feed is deterministic (fixed seed), so the
# algo and equivalence results are identical on every run; the latency numbers depend on your machine.
set -euo pipefail
cd "$(dirname "$0")"
./build.sh
./build/tests
./build/engine info | tee results/info.txt
./build/engine gen --n 2000000 --seed 1 --out data/feed.bin
./build/engine parse data/feed.bin | tee results/parse.txt
./build/engine book  data/feed.bin | tee results/book.txt
./build/engine queue               | tee results/queue.txt
./build/engine replay data/feed.bin | tee results/replay.txt
./build/engine algos data/feed.bin | tee results/algos.txt
