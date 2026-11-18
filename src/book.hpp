// Order book = (order table) + (price levels).
//   * OrderBook<Levels> owns the id -> order table and the event logic (add / execute / cancel /
//     delete / replace). It is the same code for both level stores.
//   * MapLevels    : the reference. Two std::map<price, qty>.
//   * LadderLevels : the fast one. A flat array indexed by price tick, plus cached best prices.
// Because only the level store differs, benchmarking the two isolates the data-structure change.
// This is an aggregated depth book (total shares per price level), which is all the algos need.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common.hpp"

namespace ll {

using Depth = std::vector<std::pair<uint32_t, uint64_t>>;  // (price, shares), best price first

struct ApplyResult {
  bool trade = false;  // true for an Execute that matched a known order
  uint32_t price = 0;  // trade price (the resting order's price)
  uint32_t qty = 0;
};

// ---------------------------------------------------------------------------------------------
class MapLevels {
 public:
  bool accepts(uint32_t) const { return true; }

  void add(Side s, uint32_t px, uint32_t q) {
    if (s == Side::Buy)
      bids_[px] += q;
    else
      asks_[px] += q;
  }
  void sub(Side s, uint32_t px, uint32_t q) {
    if (s == Side::Buy)
      sub_from(bids_, px, q);
    else
      sub_from(asks_, px, q);
  }
  bool best(Side s, uint32_t& px, uint64_t& q) const {
    if (s == Side::Buy) return best_of(bids_, px, q);
    return best_of(asks_, px, q);
  }
  // Visit levels of side `s` from best outward; stop when f returns false.
  template <class F>
  void walk(Side s, F&& f) const {
    if (s == Side::Buy) {
      for (auto& [px, q] : bids_)
        if (!f(px, q)) break;
    } else {
      for (auto& [px, q] : asks_)
        if (!f(px, q)) break;
    }
  }
  Depth depth(Side s) const {
    Depth d;
    walk(s, [&](uint32_t px, uint64_t q) {
      d.emplace_back(px, q);
      return true;
    });
    return d;
  }

 private:
  template <class M>
  static void sub_from(M& m, uint32_t px, uint32_t q) {
    auto it = m.find(px);
    if (it == m.end()) return;
    if (it->second <= q)
      m.erase(it);
    else
      it->second -= q;
  }
  template <class M>
  static bool best_of(const M& m, uint32_t& px, uint64_t& q) {
    if (m.empty()) return false;
    px = m.begin()->first;
    q = m.begin()->second;
    return true;
  }
  std::map<uint32_t, uint64_t, std::greater<uint32_t>> bids_;  // highest price first
  std::map<uint32_t, uint64_t> asks_;                          // lowest price first
};

// ---------------------------------------------------------------------------------------------
// Prices in [min_px, max_px] on a grid of Tick (1/10000 dollar units; 100 = one cent).
// Index of a price = (price - min_px) / Tick. Prices outside the range or off the grid are rejected.
template <uint32_t Tick = 100>
class LadderLevels {
 public:
  LadderLevels(uint32_t min_px, uint32_t max_px)
      : min_(min_px),
        n_(int64_t((max_px - min_px) / Tick) + 1),
        qty_{std::vector<uint64_t>(size_t(n_), 0), std::vector<uint64_t>(size_t(n_), 0)},
        best_bid_(-1),
        best_ask_(n_) {}

  bool accepts(uint32_t px) const {
    if (px < min_) return false;
    uint32_t d = px - min_;
    return d % Tick == 0 && int64_t(d / Tick) < n_;
  }

  void add(Side s, uint32_t px, uint32_t q) {
    int64_t i = idx(px);
    qty_[int(s)][size_t(i)] += q;
    if (s == Side::Buy) {
      if (i > best_bid_) best_bid_ = i;
    } else {
      if (i < best_ask_) best_ask_ = i;
    }
  }
  void sub(Side s, uint32_t px, uint32_t q) {
    int64_t i = idx(px);
    uint64_t& slot = qty_[int(s)][size_t(i)];
    slot = (slot > q) ? slot - q : 0;
    if (slot != 0) return;
    // The best level may just have emptied: walk to the next non-empty level.
    if (s == Side::Buy && i == best_bid_) {
      while (best_bid_ >= 0 && qty_[0][size_t(best_bid_)] == 0) --best_bid_;
    } else if (s == Side::Sell && i == best_ask_) {
      while (best_ask_ < n_ && qty_[1][size_t(best_ask_)] == 0) ++best_ask_;
    }
  }
  bool best(Side s, uint32_t& px, uint64_t& q) const {
    if (s == Side::Buy) {
      if (best_bid_ < 0) return false;
      px = price(best_bid_);
      q = qty_[0][size_t(best_bid_)];
    } else {
      if (best_ask_ >= n_) return false;
      px = price(best_ask_);
      q = qty_[1][size_t(best_ask_)];
    }
    return true;
  }
  template <class F>
  void walk(Side s, F&& f) const {
    if (s == Side::Buy) {
      for (int64_t i = best_bid_; i >= 0; --i)
        if (uint64_t q = qty_[0][size_t(i)]; q != 0 && !f(price(i), q)) break;
    } else {
      for (int64_t i = best_ask_; i < n_; ++i)
        if (uint64_t q = qty_[1][size_t(i)]; q != 0 && !f(price(i), q)) break;
    }
  }
  Depth depth(Side s) const {
    Depth d;
    walk(s, [&](uint32_t px, uint64_t q) {
      d.emplace_back(px, q);
      return true;
    });
    return d;
  }

 private:
  int64_t idx(uint32_t px) const { return int64_t((px - min_) / Tick); }
  uint32_t price(int64_t i) const { return min_ + uint32_t(i) * Tick; }

  uint32_t min_;
  int64_t n_;
  std::vector<uint64_t> qty_[2];  // [0] = bids, [1] = asks
  int64_t best_bid_;              // index of best bid, -1 if no bids
  int64_t best_ask_;              // index of best ask, n_ if no asks
};

// ---------------------------------------------------------------------------------------------
template <class Levels>
class OrderBook {
 public:
  explicit OrderBook(Levels l) : levels_(std::move(l)) { orders_.reserve(1 << 16); }

  ApplyResult apply(const Event& e) {
    ApplyResult r;
    switch (e.type) {
      case EvType::Add:
        add_order(e.id, e.side, e.price, e.qty);
        break;
      case EvType::Execute: {
        auto it = orders_.find(e.id);
        if (it == orders_.end()) {
          ++unknown_;
          break;
        }
        Order& o = it->second;
        uint32_t q = e.qty < o.qty ? e.qty : o.qty;
        levels_.sub(o.side, o.price, q);
        r = {true, o.price, q};
        o.qty -= q;
        if (o.qty == 0) orders_.erase(it);
        break;
      }
      case EvType::Cancel: {
        auto it = orders_.find(e.id);
        if (it == orders_.end()) {
          ++unknown_;
          break;
        }
        Order& o = it->second;
        uint32_t q = e.qty < o.qty ? e.qty : o.qty;
        levels_.sub(o.side, o.price, q);
        o.qty -= q;
        if (o.qty == 0) orders_.erase(it);
        break;
      }
      case EvType::Delete: {
        auto it = orders_.find(e.id);
        if (it == orders_.end()) {
          ++unknown_;
          break;
        }
        levels_.sub(it->second.side, it->second.price, it->second.qty);
        orders_.erase(it);
        break;
      }
      case EvType::Replace: {
        auto it = orders_.find(e.id);
        if (it == orders_.end()) {
          ++unknown_;
          break;
        }
        Side s = it->second.side;
        levels_.sub(s, it->second.price, it->second.qty);
        orders_.erase(it);
        add_order(e.new_id, s, e.price, e.qty);
        break;
      }
    }
    return r;
  }

  bool best_bid(uint32_t& px, uint64_t& q) const { return levels_.best(Side::Buy, px, q); }
  bool best_ask(uint32_t& px, uint64_t& q) const { return levels_.best(Side::Sell, px, q); }
  const Levels& levels() const { return levels_; }
  size_t live_orders() const { return orders_.size(); }
  uint64_t rejected() const { return rejected_; }
  uint64_t unknown() const { return unknown_; }

  // FNV-1a over the full depth of both sides. Equal hash <=> equal book (for our purposes).
  uint64_t state_hash() const {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t v) {
      h ^= v;
      h *= 1099511628211ull;
    };
    for (Side s : {Side::Buy, Side::Sell}) {
      mix(uint64_t(s));
      for (auto& [px, q] : levels_.depth(s)) {
        mix(px);
        mix(q);
      }
    }
    return h;
  }

 private:
  struct Order {
    uint32_t price;
    uint32_t qty;
    Side side;
  };

  void add_order(uint64_t id, Side s, uint32_t px, uint32_t q) {
    if (q == 0 || !levels_.accepts(px)) {
      ++rejected_;
      return;
    }
    if (!orders_.emplace(id, Order{px, q, s}).second) {  // duplicate id
      ++rejected_;
      return;
    }
    levels_.add(s, px, q);
  }

  Levels levels_;
  std::unordered_map<uint64_t, Order> orders_;
  uint64_t rejected_ = 0;
  uint64_t unknown_ = 0;
};

using MapBook = OrderBook<MapLevels>;
using LadderBook = OrderBook<LadderLevels<100>>;

// The synthetic feed lives in $80.00 .. $120.00.
inline LadderBook make_ladder_book() { return LadderBook(LadderLevels<100>(800000, 1200000)); }
inline MapBook make_map_book() { return MapBook(MapLevels()); }

}  // namespace ll
