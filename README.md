# lowlat-engine

A small, venue-agnostic market-data and execution engine in C++20, with latency measured at every stage.

Pipeline: **ITCH 5.0 parser -> order book -> lock-free SPSC queue -> TWAP / VWAP / POV execution algos**, all
driven by a **deterministic recorder/replayer** and timed with a calibrated `rdtscp` harness.

Single instrument, no external dependencies, header-only apart from `src/main.cpp`.

## Build and run

Needs g++ >= 10 (tested with g++ 16.2 from MSYS2 UCRT64 on Windows 11).

```bash
./run_all.sh        # build, run the tests, generate the feed, run every stage, save output to results/
```

Or step by step:

```bash
./build.sh
./build/tests
./build/engine gen --n 2000000 --seed 1 --out data/feed.bin
./build/engine parse data/feed.bin
./build/engine book  data/feed.bin
./build/engine queue
./build/engine replay data/feed.bin
./build/engine algos data/feed.bin
```

The feed is synthetic and seeded, so the equivalence and algo results are identical on every run. The latency
numbers depend on your machine.

## Layout

| File | What it does |
| --- | --- |
| `src/common.hpp` | `Event`: the venue-neutral message every adapter produces. |
| `src/itch.hpp` | ITCH 5.0 parser and encoder (Add, Execute, Cancel, Delete, Replace). Big-endian decoding. |
| `src/book.hpp` | `OrderBook<Levels>`: order table + event logic. `MapLevels` (reference, `std::map`) and `LadderLevels` (flat array indexed by price tick). |
| `src/spsc_queue.hpp` | Lock-free SPSC ring buffer with cache-line padding, and a mutex-based queue with the same interface. |
| `src/recorder.hpp` | Recorder (writes length-prefixed frames) and Replayer (reads them back in file order). |
| `src/feed_gen.hpp` | Deterministic synthetic ITCH feed, always uncrossed, U-shaped intensity. |
| `src/algo.hpp` | TWAP, VWAP, POV. |
| `src/sim.hpp` | Replay simulator with a book-walking fill model and slippage metrics. |
| `src/timing.hpp` | `rdtscp` timer, TSC calibration, percentile summaries, core pinning. |
| `tests/tests.cpp` | 518 checks: wire format, book semantics on both stores, equivalence, queues, algos, determinism. |

## Design notes

**Timing.** Latency is read with `rdtscp` (preceded by an `lfence` at the start of a region) and converted with a TSC
frequency measured over 200 ms against `steady_clock`. The cost of an empty timer pair is measured (median of 100k)
and subtracted from every sample. Every stage reports p50 / p99 / p99.9 / max and throughput.

**Order book.** The same `OrderBook` code runs on both level stores, so the benchmark isolates one change: `std::map`
(a tree of heap nodes) versus a flat array indexed by `(price - min) / tick`. The ladder keeps the best bid and best
ask as indices and only scans when the best level empties. Prices outside the ladder range or off the tick grid are
rejected and counted. The order table (`id -> price, qty, side`) is a `std::unordered_map` in both, which is why the
end-to-end gain is smaller than the level-store gain alone would suggest.

**Equivalence check.** `engine book` replays the full feed through both books, compares best bid/ask and every trade
after every event, and compares the full depth of both sides (a hash) every 10,000 events and at the end.

**SPSC queue.** Power-of-two capacity so the wrap is a mask. The producer's and consumer's variables each sit on their
own 64-byte line to avoid false sharing, and each side keeps a cached copy of the other's index so the shared atomic is
only re-read when the queue looks full or empty. `push` uses release and `pop` uses acquire, which is the entire
synchronisation protocol. Benchmarked against a `std::mutex` ring buffer.

**Replay.** Frames are `[2-byte big-endian length][message]` (the Nasdaq BinaryFILE framing; an 8-byte magic header
is written and optional on read). Replay walks the frames in file order and the only time source anywhere is the
exchange timestamp inside each message. Replaying the same file twice produces the same fingerprint of every order
and fill.

**Algos.** Each algo answers "how many shares in total should be done by now?" once per slice (100 ms here); the base
class turns that into "how many to send now".
- TWAP: linear in time.
- VWAP: follows the market's cumulative volume curve, built from a pre-pass over the same feed.
- POV: 10% of market volume seen since the window opened; not forced to complete.

**Fill model.** A child order is a market order that walks the opposite side of the book from the best level outward,
filling at each level's displayed price. A shortfall (not enough depth) carries into the next slice.

## Results

Machine: Intel Core i5-1335U (12 logical cores, hybrid P/E design), Windows 11, g++ 16.2 `-O2`, TSC 2.496 GHz,
timer overhead about 20 ns. Feed: 2,000,000 synthetic messages (seed 1). Full output is in `results/`.

| Stage | Throughput | p50 | p99 | p99.9 |
| --- | --- | --- | --- | --- |
| ITCH parse | 82.2 M msg/s | 15.2 ns | 41.3 ns | 126 ns |
| Book, `std::map` reference | 14.1 M events/s | 72 ns | 201 ns | 352 ns |
| Book, price ladder | 22.2 M events/s | 53 ns | 174 ns | 303 ns |
| SPSC queue (hand-off latency) | 109.9 M msg/s | 206 ns | 236 ns | 45 us |
| Mutex queue (hand-off latency) | 6.7 M msg/s | 895 ns | 1.8 us | 38 us |
| Replay (file frame -> parse -> book) | 11.5 M msg/s | 62 ns | 189 ns | 344 ns |
| Algo tick-to-decision (every event) | n/a | 51 ns | 184 ns | 348 ns |
| Algo tick-to-order (events that sent an order) | n/a | 107 ns | 565 ns | 1,023 ns |

Queue latency is measured with the producer paced at one message per 5 us; throughput is 5M messages pushed flat out.
Tail figures (p99.9 and max) include OS scheduling noise: threads are pinned to cores and run at raised priority, but
this is a desktop Windows machine with other processes running.

Execution quality (buy orders over a 30 s window, 300 slices, positive bps = worse than the benchmark):

| Parent shares | TWAP vs arrival | VWAP vs arrival | POV (10%) vs arrival | POV filled |
| --- | --- | --- | --- | --- |
| 10,000 | 0.20 bps | 0.34 bps | 0.97 bps | 100% |
| 100,000 | 0.21 bps | 0.35 bps | 2.00 bps | 100% |
| 1,000,000 | 0.28 bps | 0.44 bps | 1.96 bps | 100% |
| 10,000,000 | 1.33 bps | 1.54 bps | 0.76 bps | 31% |

Reading it: cost rises with order size because bigger slices walk further into the book. POV front-loads when the
parent is small relative to market volume, so it pays more than the time-sliced algos, and at 10M shares it only
completes 31% because the 10% cap binds. Arrival is the mid price when the window opened.

## Profiling

`docs/profiling.md` and `scripts/profile.sh` hold the gprof profile that motivated the ladder: in the `std::map` book
about a third of the time is tree insert, erase and allocation, which the flat array removes. The order table
(`std::unordered_map`) is what remains.

## Limitations

- The feed is synthetic: one instrument, simple random order flow, no real queue dynamics. Numbers describe this
  engine on this feed, not a live market.
- The fill model has no market impact feedback: our fills do not remove liquidity or move the replayed book.
- The VWAP volume curve is built from the same feed it then trades ("in-sample"). A real desk would use a historical
  average curve.
- The book stores aggregated depth per price level, not per-order queue position.
- Single instrument and a single thread per stage; the queue benchmark is the only multi-threaded test.
- Latency tails on a non-isolated desktop OS are noisy; read p50 and p99 first.
