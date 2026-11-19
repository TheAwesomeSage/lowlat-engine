// Command-line front end. One binary, one subcommand per stage:
//   engine info                      timer calibration and machine summary
//   engine gen   [--n N] [--seed S] [--out FILE]
//   engine parse [FILE]              ITCH parser: throughput and per-message latency
//   engine book  [FILE]              map book vs price-ladder book + equivalence check
//   engine queue [--cores A,B]       SPSC queue vs mutex queue
//   engine replay [FILE]             recorded file -> parse -> book, with a determinism check
//   engine profile map|ladder [FILE] one book only, for use under a profiler
//   engine algos [FILE]             TWAP / VWAP / POV: slippage, tick-to-order latency, determinism
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "algo.hpp"
#include "book.hpp"
#include "feed_gen.hpp"
#include "itch.hpp"
#include "recorder.hpp"
#include "sim.hpp"
#include "spsc_queue.hpp"
#include "timing.hpp"

using namespace ll;
using sc = std::chrono::steady_clock;

static const char* kDefaultFeed = "data/feed.bin";

static double secs(sc::time_point a, sc::time_point b) { return std::chrono::duration<double>(b - a).count(); }

static const char* arg_value(int argc, char** argv, const char* key, const char* def) {
  for (int i = 2; i + 1 < argc; ++i)
    if (std::strcmp(argv[i], key) == 0) return argv[i + 1];
  return def;
}
static const char* feed_path(int argc, char** argv) {
  return (argc >= 3 && argv[2][0] != '-') ? argv[2] : kDefaultFeed;
}

static bool load_or_complain(Replayer& rp, const char* path) {
  if (!rp.load(path)) {
    std::fprintf(stderr, "cannot read %s (run: engine gen)\n", path);
    return false;
  }
  return true;
}

static void parse_cores(int argc, char** argv, int& a, int& b) {
  unsigned hc = std::thread::hardware_concurrency();
  a = hc >= 6 ? 2 : -1;  // logical cores 2 and 4 are different physical cores on a typical SMT CPU
  b = hc >= 6 ? 4 : -1;
  const char* s = arg_value(argc, argv, "--cores", nullptr);
  if (s) std::sscanf(s, "%d,%d", &a, &b);
}

// ---------------------------------------------------------------------------------------------
static int cmd_info() {
  Clock clk = Clock::calibrate();
  std::printf("TSC frequency      : %.4f GHz (calibrated over 200 ms)\n", clk.ghz);
  std::printf("timer overhead     : %llu cycles = %.1f ns (subtracted from every sample)\n",
              (unsigned long long)clk.overhead, clk.ns(clk.overhead));
  std::printf("logical cores      : %u\n", std::thread::hardware_concurrency());
  return 0;
}

static int cmd_gen(int argc, char** argv) {
  GenConfig cfg;
  cfg.n = size_t(std::atoll(arg_value(argc, argv, "--n", "2000000")));
  cfg.seed = uint64_t(std::atoll(arg_value(argc, argv, "--seed", "1")));
  const char* out = arg_value(argc, argv, "--out", kDefaultFeed);
  uint64_t n = generate_feed(out, cfg);
  if (n == 0) {
    std::fprintf(stderr, "cannot write %s (does the folder exist?)\n", out);
    return 1;
  }
  std::printf("wrote %llu messages to %s (seed %llu)\n", (unsigned long long)n, out, (unsigned long long)cfg.seed);
  return 0;
}

// ---------------------------------------------------------------------------------------------
static int cmd_parse(int argc, char** argv) {
  Replayer rp;
  if (!load_or_complain(rp, feed_path(argc, argv))) return 1;
  Clock clk = Clock::calibrate();
  const size_t n = rp.size();
  std::printf("parser: %zu messages, TSC %.3f GHz, timer overhead %.1f ns\n\n", n, clk.ghz, clk.ns(clk.overhead));

  // Throughput: tight loop, no per-message timers (timers would distort it). Best of 5 runs.
  double best = 1e30;
  uint64_t sink = 0;
  for (int rep = 0; rep < 5; ++rep) {
    auto a = sc::now();
    for (size_t i = 0; i < n; ++i) {
      Event e;
      if (itch::parse(rp.msg(i), rp.len(i), e) == itch::Result::Ok) sink += e.id + e.qty + e.price;
    }
    best = std::min(best, secs(a, sc::now()));
  }

  // Latency: time every single parse() call with rdtscp.
  std::vector<uint64_t> cyc(n);
  for (size_t i = 0; i < n; ++i) {
    Event e;
    uint64_t t0 = t_begin();
    itch::Result r = itch::parse(rp.msg(i), rp.len(i), e);
    uint64_t t1 = t_end();
    cyc[i] = t1 - t0;
    sink += uint64_t(r) + e.id;
  }
  Percentiles p = summarize(cyc, clk);
  print_header();
  print_row("itch parse", n, double(n) / best, p);
  std::printf("\nthroughput = %.2f M msg/s   p99 = %.1f ns per message   (checksum %llu)\n", double(n) / best / 1e6,
              p.p99, (unsigned long long)sink);
  return 0;
}

// ---------------------------------------------------------------------------------------------
template <class MakeBook>
static void bench_book(const char* name, const std::vector<Event>& evs, const Clock& clk, MakeBook make) {
  double best = 1e30;
  uint64_t sink = 0;
  for (int rep = 0; rep < 3; ++rep) {
    auto b = make();
    auto a = sc::now();
    for (const Event& e : evs) sink += b.apply(e).qty;
    best = std::min(best, secs(a, sc::now()));
  }
  auto b = make();
  std::vector<uint64_t> cyc(evs.size());
  for (size_t i = 0; i < evs.size(); ++i) {
    uint64_t t0 = t_begin();
    sink += b.apply(evs[i]).qty;
    cyc[i] = t_end() - t0;
  }
  print_row(name, evs.size(), double(evs.size()) / best, summarize(cyc, clk));
  if (sink == 1) std::printf(" ");  // keep `sink` alive
}

static int cmd_book(int argc, char** argv) {
  Replayer rp;
  if (!load_or_complain(rp, feed_path(argc, argv))) return 1;
  Clock clk = Clock::calibrate();
  std::vector<Event> evs = rp.parse_all();
  std::printf("book: %zu events\n\n", evs.size());

  print_header();
  bench_book("map book (reference)", evs, clk, [] { return make_map_book(); });
  bench_book("ladder book", evs, clk, [] { return make_ladder_book(); });

  // Equivalence: replay the whole feed through both books. Compare best bid/ask after EVERY event
  // and the full depth of both sides every 10,000 events and at the end.
  MapBook ref = make_map_book();
  LadderBook fast = make_ladder_book();
  size_t mismatches = 0, depth_checks = 0;
  for (size_t i = 0; i < evs.size(); ++i) {
    ApplyResult a = ref.apply(evs[i]);
    ApplyResult b = fast.apply(evs[i]);
    uint32_t p1 = 0, p2 = 0;
    uint64_t q1 = 0, q2 = 0;
    bool h1 = ref.best_bid(p1, q1), h2 = fast.best_bid(p2, q2);
    if (h1 != h2 || (h1 && (p1 != p2 || q1 != q2))) ++mismatches;
    h1 = ref.best_ask(p1, q1);
    h2 = fast.best_ask(p2, q2);
    if (h1 != h2 || (h1 && (p1 != p2 || q1 != q2))) ++mismatches;
    if (a.trade != b.trade || a.price != b.price || a.qty != b.qty) ++mismatches;
    if (i % 10000 == 0 || i + 1 == evs.size()) {
      ++depth_checks;
      if (ref.state_hash() != fast.state_hash()) ++mismatches;
    }
  }
  std::printf("\nequivalence vs reference book: %zu events, %zu full-depth checks, %zu mismatches -> %s\n", evs.size(),
              depth_checks, mismatches, mismatches == 0 ? "IDENTICAL" : "FAILED");
  std::printf("rejected by ladder (out of range / off tick): %llu, unknown ids: %llu\n",
              (unsigned long long)fast.rejected(), (unsigned long long)fast.unknown());
  return mismatches == 0 ? 0 : 2;
}

// ---------------------------------------------------------------------------------------------
struct Msg {
  uint64_t send_tsc;
  uint64_t seq;
};

template <class Q>
static void run_queue(const char* name, const Clock& clk, int core_p, int core_c) {
  constexpr size_t kThroughputN = 5000000, kLatencyN = 200000;
  bool ordered = true;
  double tput = 0;

  // 1) Throughput: producer pushes as fast as it can.
  {
    auto q = std::make_unique<Q>();
    std::thread prod([&] {
      raise_priority();
      if (core_p >= 0) pin_to_core(core_p);
      for (uint64_t i = 0; i < kThroughputN; ++i) {
        Msg m{0, i};
        while (!q->push(m)) {
        }
      }
    });
    raise_priority();
    if (core_c >= 0) pin_to_core(core_c);
    auto a = sc::now();
    Msg m;
    for (uint64_t i = 0; i < kThroughputN; ++i) {
      while (!q->pop(m)) {
      }
      if (m.seq != i) ordered = false;
    }
    double s = secs(a, sc::now());
    prod.join();
    tput = double(kThroughputN) / s;
  }

  // 2) Latency: producer paced at one message per 5 us so the queue stays near-empty and we
  //    measure the hand-off (push -> pop) cost, not time spent waiting in a backlog.
  {
    auto q = std::make_unique<Q>();
    std::vector<uint64_t> lat;
    lat.reserve(kLatencyN);
    const uint64_t gap = uint64_t(5000.0 * clk.ghz);
    std::thread prod([&] {
      raise_priority();
      if (core_p >= 0) pin_to_core(core_p);
      uint64_t next = t_end() + gap;
      for (uint64_t i = 0; i < kLatencyN; ++i) {
        while (t_end() < next) {
        }
        next += gap;
        Msg m{t_end(), i};
        while (!q->push(m)) {
        }
      }
    });
    raise_priority();
    if (core_c >= 0) pin_to_core(core_c);
    Msg m;
    for (uint64_t i = 0; i < kLatencyN; ++i) {
      while (!q->pop(m)) {
      }
      lat.push_back(t_end() - m.send_tsc);
    }
    prod.join();
    print_row(name, kLatencyN, tput, summarize(lat, clk));
    if (!ordered) std::printf("  ^ FIFO order BROKEN in %s\n", name);
  }
}

static int cmd_queue(int argc, char** argv) {
  Clock clk = Clock::calibrate();
  int a, b;
  parse_cores(argc, argv, a, b);
  std::printf("queue: producer on core %d, consumer on core %d (-1 = not pinned)\n", a, b);
  std::printf("throughput/s = 5M messages pushed flat out; latency = push() to pop() returning, producer paced at 5 us\n");
  std::printf("(latency uses the TSC across two cores, which needs an invariant TSC; FIFO order is checked)\n\n");
  print_header();
  run_queue<SpscQueue<Msg, 65536>>("spsc lock-free", clk, a, b);
  run_queue<MutexQueue<Msg, 65536>>("mutex queue (baseline)", clk, a, b);
  return 0;
}

// ---------------------------------------------------------------------------------------------
static int cmd_algos(int argc, char** argv) {
  Replayer rp;
  if (!load_or_complain(rp, feed_path(argc, argv))) return 1;
  Clock clk = Clock::calibrate();
  std::vector<Event> evs = rp.parse_all();

  // Window: start after 50k warm-up events, run up to 300 slices of 100 ms (30 s of exchange time).
  const uint64_t slice = 100000000ull;
  size_t warm = std::min<size_t>(50000, evs.size() / 10);
  uint64_t start = evs[warm].ts_ns;
  uint64_t last = evs.back().ts_ns;
  uint64_t nslices = std::min<uint64_t>(300, (last - start) / slice);
  uint64_t end = start + nslices * slice;
  std::printf("algos: %zu events, window %.1f s of exchange time (%llu slices), buy side\n\n", evs.size(),
              double(end - start) / 1e9, (unsigned long long)nslices);

  VolumeProfile prof = build_profile(evs, start, end, 30);

  std::printf("%-6s %12s %8s %10s %10s %14s %14s %9s\n", "algo", "parent", "filled", "avg px", "mkt VWAP",
              "slip vs arr", "slip vs VWAP", "children");
  std::printf("%-6s %12s %8s %10s %10s %14s %14s %9s\n", "", "shares", "%", "$", "$", "bps", "bps", "");
  const uint64_t sizes[] = {10000, 100000, 1000000, 10000000};
  std::vector<uint64_t> all_ticks, all_orders;
  for (uint64_t sz : sizes) {
    for (int k = 0; k < 3; ++k) {
      Params p;
      p.side = Side::Buy;
      p.qty = sz;
      p.start_ns = start;
      p.end_ns = end;
      p.slice_ns = slice;
      std::unique_ptr<Algo> algo;
      if (k == 0) {
        algo = std::make_unique<Twap>(p);
      } else if (k == 1) {
        algo = std::make_unique<Vwap>(p, prof);
      } else {
        p.force_complete = false;  // POV may finish early or not at all
        algo = std::make_unique<Pov>(p, 0.10);
      }
      RunResult r = run_algo(evs, *algo);
      std::printf("%-6s %12llu %7.1f%% %10.4f %10.4f %14.2f %14.2f %9llu\n", algo->name(), (unsigned long long)sz,
                  100.0 * double(r.filled) / double(sz), r.avg_px, r.market_vwap, r.slip_arrival_bps,
                  r.slip_vwap_bps, (unsigned long long)r.children);
      all_ticks.insert(all_ticks.end(), r.tick_cycles.begin(), r.tick_cycles.end());
      all_orders.insert(all_orders.end(), r.order_cycles.begin(), r.order_cycles.end());
    }
    std::printf("\n");
  }
  std::printf("slippage: positive bps = we paid more than the benchmark (arrival = mid when the window opened).\n");
  std::printf("POV is not forced to complete; its 'filled %%' shows what the participation cap allowed.\n");
  std::printf("The table above includes market impact feedback: shares we take stay taken, so later slices walk deeper.\n\n");

  // Same TWAP orders with and without impact feedback: the difference is the cost of our own footprint.
  std::printf("TWAP slippage vs arrival, with and without impact feedback:\n");
  std::printf("%12s %14s %14s %10s\n", "parent", "no impact bps", "impact bps", "added bps");
  for (uint64_t sz : sizes) {
    Params p;
    p.side = Side::Buy;
    p.qty = sz;
    p.start_ns = start;
    p.end_ns = end;
    p.slice_ns = slice;
    Twap a(p), b(p);
    double s0 = run_algo(evs, a, false).slip_arrival_bps, s1 = run_algo(evs, b, true).slip_arrival_bps;
    std::printf("%12llu %14.2f %14.2f %10.2f\n", (unsigned long long)sz, s0, s1, s1 - s0);
  }
  std::printf("\n");

  print_header();
  size_t nt = all_ticks.size(), no = all_orders.size();
  print_row("tick-to-decision (all)", nt, 0, summarize(all_ticks, clk));
  print_row("tick-to-order", no, 0, summarize(all_orders, clk));
  std::printf("\ntick-to-decision = book update + algo.decide() for every market event in the window.\n");
  std::printf("tick-to-order    = the same, plus the book-walking fill, for events that sent an order.\n");

  // Determinism: run the same configuration twice, fingerprints must be identical.
  Params p;
  p.qty = 100000;
  p.start_ns = start;
  p.end_ns = end;
  p.slice_ns = slice;
  Twap t1(p), t2(p);
  uint64_t h1 = run_algo(evs, t1).hash, h2 = run_algo(evs, t2).hash;
  std::printf("\ndeterminism: replay #1 hash %016llx, replay #2 hash %016llx -> %s\n", (unsigned long long)h1,
              (unsigned long long)h2, h1 == h2 ? "IDENTICAL" : "DIFFERENT");
  return h1 == h2 ? 0 : 2;
}

// ---------------------------------------------------------------------------------------------
// Replay stage: the recorded file is read back frame by frame, each frame is parsed and applied to
// the ladder book. This is the recorder/replayer path end to end. Two passes must give the same
// final book (determinism).
static int cmd_replay(int argc, char** argv) {
  Replayer rp;
  if (!load_or_complain(rp, feed_path(argc, argv))) return 1;
  Clock clk = Clock::calibrate();
  const size_t n = rp.size();
  std::printf("replay: %zu frames\n\n", n);

  auto pass = [&](std::vector<uint64_t>* cyc) {
    LadderBook b = make_ladder_book();
    for (size_t i = 0; i < n; ++i) {
      Event e;
      uint64_t t0 = t_begin();
      if (itch::parse(rp.msg(i), rp.len(i), e) == itch::Result::Ok) b.apply(e);
      if (cyc) (*cyc)[i] = t_end() - t0;
    }
    return b.state_hash();
  };

  double best = 1e30;
  uint64_t h1 = 0;
  for (int rep = 0; rep < 3; ++rep) {
    auto a = sc::now();
    h1 = pass(nullptr);
    best = std::min(best, secs(a, sc::now()));
  }
  std::vector<uint64_t> cyc(n);
  uint64_t h2 = pass(&cyc);
  print_header();
  print_row("replay (parse + book)", n, double(n) / best, summarize(cyc, clk));
  std::printf("\ndeterminism: final book hash pass A %016llx, pass B %016llx -> %s\n", (unsigned long long)h1,
              (unsigned long long)h2, h1 == h2 ? "IDENTICAL" : "DIFFERENT");
  return h1 == h2 ? 0 : 2;
}

// ---------------------------------------------------------------------------------------------
// Runs ONLY one book implementation for several passes, so a profiler's output is not a mix of
// both. Usage: engine profile map|ladder [FILE] [PASSES]   (see docs/profiling.md and scripts/profile.sh)
static int cmd_profile(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: engine profile map|ladder [FILE]\n");
    return 1;
  }
  bool use_map = std::strcmp(argv[2], "map") == 0;
  Replayer rp;
  if (!load_or_complain(rp, argc >= 4 ? argv[3] : kDefaultFeed)) return 1;
  std::vector<Event> evs = rp.parse_all();
  const int passes = argc >= 5 ? std::atoi(argv[4]) : 8;
  uint64_t sink = 0;
  auto a = sc::now();
  for (int rep = 0; rep < passes; ++rep) {
    if (use_map) {
      MapBook b = make_map_book();
      for (const Event& e : evs) sink += b.apply(e).qty;
    } else {
      LadderBook b = make_ladder_book();
      for (const Event& e : evs) sink += b.apply(e).qty;
    }
  }
  double s = secs(a, sc::now());
  std::printf("%s book, %d passes x %zu events: %.2f M events/s (checksum %llu)\n", use_map ? "map" : "ladder",
              passes, evs.size(), double(size_t(passes) * evs.size()) / s / 1e6, (unsigned long long)sink);
  return 0;
}

// ---------------------------------------------------------------------------------------------
int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: engine <info|gen|parse|book|queue|replay|algos> [options]\n");
    return 1;
  }
  std::string c = argv[1];
  if (c == "info") return cmd_info();
  if (c == "gen") return cmd_gen(argc, argv);
  if (c == "parse") return cmd_parse(argc, argv);
  if (c == "book") return cmd_book(argc, argv);
  if (c == "queue") return cmd_queue(argc, argv);
  if (c == "replay") return cmd_replay(argc, argv);
  if (c == "profile") return cmd_profile(argc, argv);
  if (c == "algos") return cmd_algos(argc, argv);
  std::fprintf(stderr, "unknown command: %s\n", c.c_str());
  return 1;
}
