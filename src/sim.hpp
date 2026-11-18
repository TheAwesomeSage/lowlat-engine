// Replay simulator: feeds market events to a book, asks the algo what to do on every event, and
// fills the algo's orders by WALKING THE BOOK.
//
// Fill model: a child order is a market order. It takes the best opposite-side level first, then the
// next one, and so on, at each level's displayed price, until it is filled or the book runs out
// (a partial fill; the shortfall carries into the next slice).
// Simplification (stated, not hidden): our own fills do not remove liquidity from the replayed book
// and do not move the market - the feed is historical, so there is no market impact feedback.
#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

#include "algo.hpp"
#include "book.hpp"
#include "timing.hpp"

namespace ll {

struct RunResult {
  uint64_t parent = 0, filled = 0, children = 0;
  double avg_px = 0;            // dollars
  double arrival_px = 0;        // mid price when the window opened, dollars
  double market_vwap = 0;       // market VWAP over the execution window, dollars
  double slip_arrival_bps = 0;  // positive = we did worse than the benchmark
  double slip_vwap_bps = 0;
  uint64_t hash = 0;                    // fingerprint of every child order and fill (determinism check)
  std::vector<uint64_t> tick_cycles;    // apply + decide, every event inside the window
  std::vector<uint64_t> order_cycles;   // apply + decide + fill walk, only events that sent an order
};

inline RunResult run_algo(const std::vector<Event>& evs, Algo& algo) {
  const Params& P = algo.params();
  LadderBook book = make_ladder_book();
  RunResult R;
  R.parent = P.qty;

  uint64_t h = 1469598103934665603ull;
  auto mix = [&](uint64_t v) {
    h ^= v;
    h *= 1099511628211ull;
  };

  bool started = false;
  uint64_t mkt_vol = 0, mkt_notional = 0, filled = 0, notional = 0;
  const Side opp = (P.side == Side::Buy) ? Side::Sell : Side::Buy;

  for (const Event& e : evs) {
    uint64_t t0 = t_begin();
    ApplyResult r = book.apply(e);
    uint64_t sent = 0;
    if (e.ts_ns >= P.start_ns) {
      if (!started) {
        started = true;
        uint32_t bp, ap;
        uint64_t q;
        if (book.best_bid(bp, q) && book.best_ask(ap, q)) R.arrival_px = (double(bp) + double(ap)) / 2.0 / 10000.0;
      }
      if (r.trade) {
        mkt_vol += r.qty;
        mkt_notional += uint64_t(r.price) * r.qty;
      }
      sent = algo.decide({e.ts_ns, mkt_vol}, filled);
    }
    uint64_t t1 = t_end();
    if (started) R.tick_cycles.push_back(t1 - t0);

    if (sent > 0) {
      uint64_t rem = sent, got = 0, notl = 0;
      book.levels().walk(opp, [&](uint32_t px, uint64_t lvl) {
        uint64_t take = std::min(rem, lvl);
        notl += take * px;
        got += take;
        rem -= take;
        return rem > 0;
      });
      uint64_t t2 = t_end();
      R.order_cycles.push_back(t2 - t0);
      filled += got;
      notional += notl;
      ++R.children;
      mix(e.ts_ns);
      mix(sent);
      mix(got);
      mix(notl);
    }
    if (started && e.ts_ns >= P.end_ns) break;
  }

  R.filled = filled;
  R.hash = h;
  if (filled > 0) R.avg_px = double(notional) / double(filled) / 10000.0;
  if (mkt_vol > 0) R.market_vwap = double(mkt_notional) / double(mkt_vol) / 10000.0;
  double sign = (P.side == Side::Buy) ? 1.0 : -1.0;
  if (R.avg_px > 0 && R.arrival_px > 0) R.slip_arrival_bps = sign * (R.avg_px - R.arrival_px) / R.arrival_px * 1e4;
  if (R.avg_px > 0 && R.market_vwap > 0) R.slip_vwap_bps = sign * (R.avg_px - R.market_vwap) / R.market_vwap * 1e4;
  return R;
}

}  // namespace ll
