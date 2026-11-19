# Profiling the order book

Question: where does the `std::map` book spend its time, and does the price-ladder array remove it?

## Method

- Tool: **gprof** (an instrumenting profiler: the compiler adds counting code to every function, and a sampler
  records which function is running every 10 ms).
- Run: `scripts/profile.sh` (Linux or WSL). It builds with `-O2 -fno-inline-functions -no-pie -pg`, then runs
  `engine profile map` and `engine profile ladder`, each replaying the 2M-message feed 40 times (80M events per book).
- `-fno-inline-functions` is deliberate: without it the compiler folds `add_order` and `MapLevels::sub` into
  `apply()` and the profile shows one big row. It also slows the code a little and shifts the proportions, so read
  the percentages as a guide to *where* the time goes, not as exact figures.
- Machine: the same i5-1335U laptop under WSL2 Ubuntu. gprof has about 10 ms resolution and adds overhead, so the
  absolute speeds it prints (13.4 vs 17.3 M events/s) are lower than the normal benchmark's.

## Result (flat profile, own functions only; range over two runs)

| `std::map` book | % of time | | Ladder book | % of time |
| --- | --- | --- | --- | --- |
| `OrderBook::apply` | 28-35 | | `OrderBook::apply` (level updates inlined in it) | 50-58 |
| `OrderBook::add_order` (includes the `std::map` insert) | 19-21 | | `unordered_map` insert | about 6 |
| `MapLevels::sub` (`std::map` find and erase) | 13-15 | | `unordered_map` find | 4-10 |
| `unordered_map` find | 2-4 | | | |

Percentages move by several points between runs because gprof samples every 10 ms; the shape does not.

gprof mislabels a few libstdc++ helper functions (for example a row named `Twap::~Twap` with a call count equal to the
hash-table insert count). They are left out above; they account for the remaining time in both columns.

## What it shows

1. In the `std::map` book, roughly a third of the time (`add_order` plus `MapLevels::sub`) is spent walking, inserting
   into and erasing from a red-black tree: pointer chasing, a heap allocation per new price level, and rebalancing.
2. In the ladder, those two rows disappear. A price becomes an array index, so a level update is one indexed add
   inside `apply()`, with no allocation and no tree walk.
3. What is left is the order table, a `std::unordered_map`. gprof shows 10-16% of the ladder's time, but it
   under-reports (see the perf cross-check below, about 42%). It is now the next obvious thing to optimise, and it is why the
   end-to-end gain is about 1.5x rather than larger.

Next step if this were pursued: replace the `unordered_map` order table with a flat open-addressing table.

## Cross-check with perf (sampling profiler)

gprof instruments the code and I had to switch inlining off for it, so I repeated the measurement with `perf`
(`scripts/perf.sh`, `perf record -F 999 -e cpu-clock`). It samples about 1000 times a second and profiles the normal
`-O2` build with inlining left on. One run per book, 40 passes over the 2M-message feed; read the numbers as "about".

| `std::map` book | % of samples | | Ladder book | % of samples |
| --- | --- | --- | --- | --- |
| `OrderBook::apply` | 21 | | `OrderBook::apply` (level updates inlined in it) | 42 |
| `MapLevels::sub` | 20 | | `unordered_map` rows (3, summed) | 42 |
| `OrderBook::add_order` (includes the tree insert) | 19 | | `malloc` / `free` / `operator new` | about 7 |
| `unordered_map` rows (2, summed) | 30 | | | |
| `malloc` / `free` / `operator new` | about 4 | | | |

- It agrees with gprof on the main point: tree insert, erase and allocation are roughly 40% of the map book, and the
  ladder removes the `sub` and `add_order` rows.
- It disagrees on the order table. gprof showed it as 10-16% of the ladder's time, but gprof mislabels the libstdc++
  hash-table helpers and those rows were left out. perf resolves them and puts the order table at about 42% of the
  ladder book. So the order table is the largest remaining cost, which strengthens the "flat hash table next" conclusion.
- With inlining on, a function's share includes code inlined into it, so the `apply` rows hide some tree and hash work.
