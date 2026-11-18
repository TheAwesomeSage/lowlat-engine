// Execution algorithms. Each one answers a single question, once per slice (the benchmark uses 100 ms):
//   "how many shares in TOTAL should be done by now?"  -> target()
// and the base class turns that into "how many shares do I send right now?" -> decide().
//   TWAP : target grows linearly with time.
//   VWAP : target follows the market's volume curve (cumulative fraction of volume by time).
//   POV  : target = participation rate x market volume seen since the start.
#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

#include "book.hpp"
#include "common.hpp"

namespace ll {

struct Params {
  Side side = Side::Buy;
  uint64_t qty = 0;                      // parent order size (shares)
  uint64_t start_ns = 0, end_ns = 0;     // execution window (exchange time)
  uint64_t slice_ns = 1000000000ull;     // decide once per slice
  bool force_complete = true;            // at window end, send whatever is left
};

struct MarketState {
  uint64_t ts_ns;
  uint64_t volume;  // shares executed in the market since the window started
};

class Algo {
 public:
  explicit Algo(const Params& p) : p_(p), next_slice_(p.start_ns + p.slice_ns) {}
  virtual ~Algo() = default;
  virtual const char* name() const = 0;

  // Called on every market event (cheap: one compare unless a slice boundary was crossed).
  // Returns the number of shares to send now; 0 means do nothing.
  uint64_t decide(const MarketState& m, uint64_t filled) {
    if (m.ts_ns < next_slice_) return 0;
    bool last = m.ts_ns >= p_.end_ns;
    while (next_slice_ <= m.ts_ns) next_slice_ += p_.slice_ns;
    uint64_t tgt = target(m);
    if (last && p_.force_complete) tgt = p_.qty;
    if (tgt > p_.qty) tgt = p_.qty;
    return tgt > filled ? tgt - filled : 0;
  }
  const Params& params() const { return p_; }

 protected:
  virtual uint64_t target(const MarketState& m) const = 0;
  double time_frac(uint64_t ts) const {
    if (ts <= p_.start_ns) return 0.0;
    if (ts >= p_.end_ns) return 1.0;
    return double(ts - p_.start_ns) / double(p_.end_ns - p_.start_ns);
  }
  Params p_;
  uint64_t next_slice_;
};

class Twap : public Algo {
 public:
  using Algo::Algo;
  const char* name() const override { return "TWAP"; }

 protected:
  uint64_t target(const MarketState& m) const override { return uint64_t(double(p_.qty) * time_frac(m.ts_ns)); }
};

// Cumulative share of the day's volume done by time-fraction t, in `buckets` equal time buckets.
// Built from a pre-pass over the same feed ("in-sample"). A real desk would use a historical
// average volume curve instead; the mechanics of following it are identical.
struct VolumeProfile {
  std::vector<double> cum;  // cum[0] = 0 ... cum[B] = 1

  double at(double t) const {
    if (cum.size() < 2) return t;
    double x = t * double(cum.size() - 1);
    size_t i = size_t(x);
    if (i >= cum.size() - 1) return 1.0;
    return cum[i] + (cum[i + 1] - cum[i]) * (x - double(i));
  }
};

inline VolumeProfile build_profile(const std::vector<Event>& evs, uint64_t start_ns, uint64_t end_ns,
                                   size_t buckets = 30) {
  LadderBook book = make_ladder_book();
  std::vector<double> vol(buckets, 0.0);
  double total = 0;
  for (const Event& e : evs) {
    ApplyResult r = book.apply(e);
    if (r.trade && e.ts_ns >= start_ns && e.ts_ns < end_ns) {
      size_t b = size_t(double(e.ts_ns - start_ns) / double(end_ns - start_ns) * double(buckets));
      if (b >= buckets) b = buckets - 1;
      vol[b] += r.qty;
      total += r.qty;
    }
  }
  VolumeProfile p;
  p.cum.assign(buckets + 1, 0.0);
  for (size_t i = 0; i < buckets; ++i) p.cum[i + 1] = p.cum[i] + (total > 0 ? vol[i] / total : 1.0 / double(buckets));
  return p;
}

class Vwap : public Algo {
 public:
  Vwap(const Params& p, VolumeProfile prof) : Algo(p), prof_(std::move(prof)) {}
  const char* name() const override { return "VWAP"; }

 protected:
  uint64_t target(const MarketState& m) const override {
    return uint64_t(double(p_.qty) * prof_.at(time_frac(m.ts_ns)));
  }

 private:
  VolumeProfile prof_;
};

class Pov : public Algo {
 public:
  Pov(const Params& p, double rate) : Algo(p), rate_(rate) {}
  const char* name() const override { return "POV"; }

 protected:
  uint64_t target(const MarketState& m) const override { return uint64_t(rate_ * double(m.volume)); }

 private:
  double rate_;
};

}  // namespace ll
