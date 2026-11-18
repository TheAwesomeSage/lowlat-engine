// Deterministic synthetic ITCH feed (one instrument). Same seed -> byte-identical file.
// It keeps its own reference book so it never produces a crossed book (best bid < best ask always),
// and message intensity follows a U-shape (busy open and close, quiet middle) so that a VWAP
// schedule has a real volume curve to follow.
#pragma once
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "book.hpp"
#include "itch.hpp"
#include "recorder.hpp"

namespace ll {

struct GenConfig {
  uint64_t seed = 1;
  size_t n = 2000000;  // number of messages
};

// xorshift64*: tiny, fast and identical on every platform (std::uniform_int_distribution is not).
class Rng {
 public:
  explicit Rng(uint64_t seed) : s_(seed ? seed : 1) {}
  uint64_t next() {
    s_ ^= s_ >> 12;
    s_ ^= s_ << 25;
    s_ ^= s_ >> 27;
    return s_ * 2685821657736338717ull;
  }
  uint64_t below(uint64_t n) { return (next() >> 11) % n; }

 private:
  uint64_t s_;
};

inline uint64_t generate_feed(const std::string& path, const GenConfig& cfg) {
  Recorder rec(path);
  if (!rec.ok()) return 0;

  struct Live {
    uint64_t id;
    Side side;
    uint32_t price, qty;
  };
  constexpr uint32_t kTick = 100;
  constexpr uint32_t kMidLo = 950000, kMidHi = 1050000;  // $95 .. $105

  Rng rng(cfg.seed);
  MapBook book = make_map_book();
  std::vector<Live> live;
  live.reserve(8192);
  uint32_t mid = 1000000;  // $100.00
  uint64_t ts = 34200ull * 1000000000ull;  // 09:30:00
  uint64_t next_id = 1;
  uint8_t buf[64];

  // A passive price `off` ticks away from mid, nudged so it can never cross the opposite side.
  auto passive_price = [&](Side s, uint32_t off) {
    uint32_t bp;
    uint64_t q;
    if (s == Side::Buy) {
      uint32_t px = mid - off * kTick;
      if (book.best_ask(bp, q) && px >= bp) px = bp - kTick;
      return px;
    }
    uint32_t px = mid + kTick + off * kTick;
    if (book.best_bid(bp, q) && px <= bp) px = bp + kTick;
    return px;
  };
  auto near_touch = [&](const Live& o) {
    uint32_t bp;
    uint64_t q;
    bool have = (o.side == Side::Buy) ? book.best_bid(bp, q) : book.best_ask(bp, q);
    if (!have) return false;
    uint32_t d = o.price > bp ? o.price - bp : bp - o.price;
    return d <= 2 * kTick;
  };
  auto remove_live = [&](size_t idx) {
    live[idx] = live.back();
    live.pop_back();
  };

  for (size_t i = 0; i < cfg.n; ++i) {
    double f = double(i) / double(cfg.n);
    double mult = 0.4 + 1.2 * (1.0 - std::fabs(2.0 * f - 1.0));  // 0.4 at the ends, 1.6 mid-day
    ts += uint64_t(double(1 + rng.below(40000)) * mult);         // mean gap ~20 us
    if (rng.below(20000) == 0) {  // mid moves one cent about every 20,000 messages (~0.4 s)
      mid = rng.below(2) ? mid + kTick : mid - kTick;
      if (mid < kMidLo) mid = kMidLo;
      if (mid > kMidHi) mid = kMidHi;
    }

    int kind;  // 0 add, 1 delete, 2 execute, 3 cancel, 4 replace
    if (live.size() < 1500) {
      kind = 0;
    } else if (live.size() > 3000) {
      kind = 1;
    } else {
      uint64_t r = rng.below(100);
      kind = r < 35 ? 0 : r < 60 ? 1 : r < 80 ? 2 : r < 90 ? 3 : 4;
    }

    Event e;
    e.ts_ns = ts;
    if (kind == 0) {
      Side s = rng.below(2) ? Side::Buy : Side::Sell;
      e.type = EvType::Add;
      e.side = s;
      e.id = next_id++;
      e.qty = uint32_t(100 * (1 + rng.below(10)));
      e.price = passive_price(s, uint32_t(rng.below(6) + rng.below(6)));
      live.push_back({e.id, s, e.price, e.qty});
    } else {
      size_t idx = size_t(rng.below(live.size()));
      if (kind == 2) {  // executions happen at the touch: look for an order near the best price
        for (int t = 0; t < 4 && !near_touch(live[idx]); ++t) idx = size_t(rng.below(live.size()));
        if (!near_touch(live[idx])) kind = 1;
      }
      Live& o = live[idx];
      if (kind == 3 && o.qty <= 100) kind = 1;
      if (kind == 1) {
        e.type = EvType::Delete;
        e.id = o.id;
        remove_live(idx);
      } else if (kind == 2) {
        e.type = EvType::Execute;
        e.id = o.id;
        e.qty = uint32_t(std::min<uint64_t>(o.qty, 100 * (1 + rng.below(5))));
        o.qty -= e.qty;
        if (o.qty == 0) remove_live(idx);
      } else if (kind == 3) {
        e.type = EvType::Cancel;
        e.id = o.id;
        e.qty = 100;
        o.qty -= 100;
      } else {
        e.type = EvType::Replace;
        e.id = o.id;
        e.new_id = next_id++;
        e.qty = uint32_t(100 * (1 + rng.below(10)));
        e.price = passive_price(o.side, uint32_t(rng.below(6) + rng.below(6)));
        o = {e.new_id, o.side, e.price, e.qty};
      }
    }
    book.apply(e);
    rec.record(buf, uint16_t(itch::encode(e, buf)));
  }
  return rec.count();
}

}  // namespace ll
