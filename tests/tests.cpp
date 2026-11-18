// Plain assert-style tests (no framework). Exit code 0 = all passed.
#include <cstdio>
#include <filesystem>
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

using namespace ll;

static int g_fail = 0, g_checks = 0;
#define CHECK(cond)                                                          \
  do {                                                                       \
    ++g_checks;                                                              \
    if (!(cond)) {                                                           \
      ++g_fail;                                                              \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);            \
    }                                                                        \
  } while (0)

static Event add(uint64_t id, Side s, uint32_t px, uint32_t q, uint64_t ts = 1) {
  Event e;
  e.type = EvType::Add;
  e.id = id;
  e.side = s;
  e.price = px;
  e.qty = q;
  e.ts_ns = ts;
  return e;
}

// ---- ITCH ------------------------------------------------------------------------------------
static void test_itch_roundtrip() {
  Event evs[5];
  evs[0] = add(0x1122334455667788ull, Side::Sell, 1234500, 777, 0x0000AABBCCDDEEull);
  evs[1].type = EvType::Execute;
  evs[1].id = 42;
  evs[1].qty = 100;
  evs[1].ts_ns = 123456789012ull;
  evs[2].type = EvType::Cancel;
  evs[2].id = 43;
  evs[2].qty = 50;
  evs[2].ts_ns = 5;
  evs[3].type = EvType::Delete;
  evs[3].id = 44;
  evs[3].ts_ns = 6;
  evs[4].type = EvType::Replace;
  evs[4].id = 45;
  evs[4].new_id = 46;
  evs[4].qty = 900;
  evs[4].price = 999900;
  evs[4].ts_ns = 7;
  for (const Event& in : evs) {
    uint8_t buf[64];
    size_t n = itch::encode(in, buf);
    Event out;
    CHECK(itch::parse(buf, n, out) == itch::Result::Ok);
    CHECK(out.type == in.type && out.id == in.id && out.ts_ns == in.ts_ns);
    CHECK(out.qty == in.qty && out.price == in.price && out.new_id == in.new_id);
    if (in.type == EvType::Add) CHECK(out.side == in.side);
    CHECK(itch::parse(buf, n - 1, out) == itch::Result::Malformed);  // truncated
  }
}

static void test_itch_wire_bytes() {
  // Hand-built Add Order: checks real big-endian offsets, independent of encode().
  uint8_t m[36] = {'A', 0, 1, 0, 0, 0x00, 0x00, 0x00, 0x00, 0x03, 0xE8,  // ts = 1000
                   0, 0, 0, 0, 0, 0, 0, 7,                               // order ref = 7
                   'B', 0, 0, 0, 100,                                     // buy 100
                   'A', 'B', 'C', ' ', ' ', ' ', ' ', ' ',                // stock
                   0, 0x0F, 0x42, 0x40};                                  // price = 1,000,000
  Event e;
  CHECK(itch::parse(m, sizeof m, e) == itch::Result::Ok);
  CHECK(e.ts_ns == 1000 && e.id == 7 && e.side == Side::Buy && e.qty == 100 && e.price == 1000000);
  uint8_t sys[12] = {'S'};
  CHECK(itch::parse(sys, sizeof sys, e) == itch::Result::Ignored);
  CHECK(itch::parse(m, 0, e) == itch::Result::Malformed);
}

// ---- Order book (runs against BOTH level stores) -----------------------------------------------
template <class B>
static void book_scenarios(B b) {
  uint32_t px;
  uint64_t q;
  CHECK(!b.best_bid(px, q) && !b.best_ask(px, q));

  b.apply(add(1, Side::Buy, 1000000, 300));
  b.apply(add(2, Side::Buy, 1000000, 200));
  b.apply(add(3, Side::Buy, 999900, 500));
  b.apply(add(4, Side::Sell, 1000100, 400));
  CHECK(b.best_bid(px, q) && px == 1000000 && q == 500);  // two orders aggregate at one level
  CHECK(b.best_ask(px, q) && px == 1000100 && q == 400);

  Event ex;
  ex.type = EvType::Execute;
  ex.id = 1;
  ex.qty = 100;
  ApplyResult r = b.apply(ex);
  CHECK(r.trade && r.price == 1000000 && r.qty == 100);
  CHECK(b.best_bid(px, q) && q == 400);
  ex.qty = 9999;  // more than the order has left: clamped to remaining 200
  r = b.apply(ex);
  CHECK(r.trade && r.qty == 200);

  Event ca;
  ca.type = EvType::Cancel;
  ca.id = 2;
  ca.qty = 50;
  b.apply(ca);  // partial cancel of order 2 (200 -> 150)
  CHECK(b.best_bid(px, q) && px == 1000000 && q == 150);

  Event de;
  de.type = EvType::Delete;
  de.id = 2;
  b.apply(de);  // last order at the best level -> best bid falls to the next level
  CHECK(b.best_bid(px, q) && px == 999900 && q == 500);

  Event rp;
  rp.type = EvType::Replace;
  rp.id = 4;
  rp.new_id = 5;
  rp.qty = 250;
  rp.price = 1000200;
  b.apply(rp);  // replace loses the old level, creates the new one, keeps the side
  CHECK(b.best_ask(px, q) && px == 1000200 && q == 250);

  uint64_t unk = b.unknown();
  b.apply(de);  // deleting an unknown id is counted and otherwise harmless
  CHECK(b.unknown() == unk + 1);
  CHECK(b.state_hash() == b.state_hash());
}

static void test_books() {
  book_scenarios(make_map_book());
  book_scenarios(make_ladder_book());

  LadderBook lb = make_ladder_book();  // out-of-range and off-tick prices are rejected, not stored
  lb.apply(add(1, Side::Buy, 100, 10));
  lb.apply(add(2, Side::Buy, 1000050, 10));
  lb.apply(add(3, Side::Buy, 1000000, 0));
  CHECK(lb.rejected() == 3 && lb.live_orders() == 0);
}

// ---- Feed, recorder, replay, equivalence -------------------------------------------------------
static std::string tmp_feed(const char* name) { return (std::filesystem::temp_directory_path() / name).string(); }

static void test_feed_and_equivalence() {
  std::string a = tmp_feed("ll_test_a.bin"), b = tmp_feed("ll_test_b.bin");
  GenConfig cfg;
  cfg.n = 200000;
  cfg.seed = 7;
  CHECK(generate_feed(a, cfg) == cfg.n);
  CHECK(generate_feed(b, cfg) == cfg.n);

  Replayer ra, rb;
  CHECK(ra.load(a) && rb.load(b));
  CHECK(ra.size() == cfg.n && rb.size() == cfg.n);
  bool same = true;  // determinism: same seed -> byte-identical frames
  for (size_t i = 0; i < ra.size() && same; ++i)
    same = ra.len(i) == rb.len(i) && std::memcmp(ra.msg(i), rb.msg(i), ra.len(i)) == 0;
  CHECK(same);

  size_t ign = 0, bad = 0;
  std::vector<Event> evs = ra.parse_all(&ign, &bad);
  CHECK(evs.size() == cfg.n && ign == 0 && bad == 0);

  MapBook ref = make_map_book();
  LadderBook fast = make_ladder_book();
  bool eq = true, uncrossed = true;
  for (size_t i = 0; i < evs.size(); ++i) {
    ApplyResult x = ref.apply(evs[i]), y = fast.apply(evs[i]);
    eq = eq && x.trade == y.trade && x.price == y.price && x.qty == y.qty;
    uint32_t bp, ap;
    uint64_t q;
    if (ref.best_bid(bp, q) && ref.best_ask(ap, q) && bp >= ap) uncrossed = false;
    if (i % 5000 == 0) eq = eq && ref.state_hash() == fast.state_hash();
  }
  CHECK(eq);
  CHECK(uncrossed);
  CHECK(ref.state_hash() == fast.state_hash());
  CHECK(fast.rejected() == 0 && fast.unknown() == 0);
  std::filesystem::remove(a);
  std::filesystem::remove(b);
}

// ---- Queues ----------------------------------------------------------------------------------
template <class Q>
static void queue_tests() {
  Q q;
  uint64_t v = 0;
  CHECK(!q.pop(v));
  for (uint64_t i = 0; i < 8; ++i) CHECK(q.push(i));
  CHECK(!q.push(99));  // full (capacity 8)
  for (uint64_t i = 0; i < 8; ++i) CHECK(q.pop(v) && v == i);
  CHECK(!q.pop(v));
  for (uint64_t i = 0; i < 100; ++i) {  // wrap-around many times
    CHECK(q.push(i));
    CHECK(q.pop(v) && v == i);
  }
  // Two threads: every item arrives, in order.
  constexpr uint64_t kN = 1000000;
  auto big = std::make_unique<Q>();
  std::thread prod([&] {
    for (uint64_t i = 0; i < kN; ++i)
      while (!big->push(i)) {
      }
  });
  bool ordered = true;
  for (uint64_t i = 0; i < kN; ++i) {
    uint64_t x;
    while (!big->pop(x)) {
    }
    if (x != i) ordered = false;
  }
  prod.join();
  CHECK(ordered);
}

// ---- Algos -----------------------------------------------------------------------------------
static void test_algos() {
  std::string path = tmp_feed("ll_test_algo.bin");
  GenConfig cfg;
  cfg.n = 400000;
  cfg.seed = 3;
  generate_feed(path, cfg);
  Replayer rp;
  CHECK(rp.load(path));
  std::vector<Event> evs = rp.parse_all();
  const uint64_t slice = 1000000000ull;
  uint64_t start = evs[20000].ts_ns;
  uint64_t nsl = std::min<uint64_t>(5, (evs.back().ts_ns - start) / slice);
  CHECK(nsl >= 2);
  Params p;
  p.qty = 20000;
  p.start_ns = start;
  p.end_ns = start + nsl * slice;
  p.slice_ns = slice;

  Twap t1(p), t2(p);
  RunResult r1 = run_algo(evs, t1), r2 = run_algo(evs, t2);
  CHECK(r1.filled == p.qty);              // TWAP is forced to complete on a deep book
  CHECK(r1.children >= nsl);              // roughly one child order per slice
  CHECK(r1.hash == r2.hash);              // deterministic replay
  CHECK(r1.avg_px > 90.0 && r1.avg_px < 110.0 && r1.arrival_px > 0);

  VolumeProfile prof = build_profile(evs, p.start_ns, p.end_ns, 10);
  CHECK(prof.cum.size() == 11 && prof.cum.front() == 0.0 && prof.cum.back() > 0.999);
  for (size_t i = 1; i < prof.cum.size(); ++i) CHECK(prof.cum[i] >= prof.cum[i - 1]);
  Vwap v(p, prof);
  CHECK(run_algo(evs, v).filled == p.qty);

  Params pp = p;
  pp.force_complete = false;
  Pov pov(pp, 0.10);
  RunResult rv = run_algo(evs, pov);
  CHECK(rv.filled <= p.qty);

  // Sizing: a bigger parent order walks deeper into the book, so it cannot be cheaper per share.
  Params big = p;
  big.qty = 5000000;
  Twap tb(big);
  RunResult rb = run_algo(evs, tb);
  CHECK(rb.slip_arrival_bps >= r1.slip_arrival_bps);
  std::filesystem::remove(path);
}

int main() {
  test_itch_roundtrip();
  test_itch_wire_bytes();
  test_books();
  test_feed_and_equivalence();
  queue_tests<SpscQueue<uint64_t, 8>>();
  queue_tests<MutexQueue<uint64_t, 8>>();
  test_algos();
  std::printf("%d checks, %d failed\n", g_checks, g_fail);
  return g_fail == 0 ? 0 : 1;
}
