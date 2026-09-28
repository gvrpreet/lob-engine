#pragma once
// Deliberately naive O(n) order book used as a test ORACLE.
// Rule: never optimise this file. Its only virtue is being obviously correct.
#include <algorithm>
#include <optional>
#include <vector>

#include "lob/types.hpp"

class RefBook {
 public:
  struct Ord { lob::OrderId id; lob::Side side; lob::Price price; lob::Qty qty; std::uint64_t seq; };

  lob::AddResult add(lob::OrderId id, lob::Side side, lob::OrderType type, lob::Price price,
                     lob::Qty qty, lob::Timestamp ts, std::vector<lob::Trade>& out) {
    using namespace lob;
    if (qty == 0 || find(id) >= 0) return AddResult::Rejected;
    const Qty original = qty;
    while (qty > 0) {
      int best = -1;
      for (int i = 0; i < static_cast<int>(orders_.size()); ++i) {
        const Ord& o = orders_[i];
        if (o.side == side) continue;
        if (type == OrderType::Limit && !(side == Side::Buy ? price >= o.price : price <= o.price)) continue;
        if (best < 0 || better(side, o, orders_[best])) best = i;
      }
      if (best < 0) break;
      Ord& m = orders_[best];
      const Qty fill = std::min(qty, m.qty);
      out.push_back(Trade{id, m.id, m.price, fill, ts});
      m.qty -= fill;
      qty -= fill;
      if (m.qty == 0) orders_.erase(orders_.begin() + best);
    }
    if (qty == 0) return AddResult::Filled;
    if (type == OrderType::Market) return AddResult::Discarded;
    orders_.push_back(Ord{id, side, price, qty, seq_++});
    return qty == original ? AddResult::Rested : AddResult::PartiallyRested;
  }

  bool cancel(lob::OrderId id) {
    const int i = find(id);
    if (i < 0) return false;
    orders_.erase(orders_.begin() + i);
    return true;
  }

  // Partial cancel: keeps the order's seq (queue position) unless it removes the order.
  bool reduce(lob::OrderId id, lob::Qty by) {
    const int i = find(id);
    if (i < 0) return false;
    if (by >= orders_[i].qty) orders_.erase(orders_.begin() + i);
    else orders_[i].qty -= by;
    return true;
  }

  // Same price and qty not raised: amend in place (keeps seq). Otherwise: remove, then add again
  // as a new limit order with the same id and side (new seq, may trade).
  lob::ModifyResult modify(lob::OrderId id, lob::Price price, lob::Qty qty, lob::Timestamp ts,
                           std::vector<lob::Trade>& out) {
    using namespace lob;
    const int i = find(id);
    if (i < 0) return ModifyResult::NotFound;
    if (qty == 0) return ModifyResult::Rejected;
    if (price == orders_[i].price && qty <= orders_[i].qty) {
      orders_[i].qty = qty;
      return ModifyResult::Amended;
    }
    const Side side = orders_[i].side;
    orders_.erase(orders_.begin() + i);
    switch (add(id, side, OrderType::Limit, price, qty, ts, out)) {
      case AddResult::Rested: return ModifyResult::Rested;
      case AddResult::PartiallyRested: return ModifyResult::PartiallyRested;
      case AddResult::Filled: return ModifyResult::Filled;
      default: return ModifyResult::Rejected;
    }
  }

  std::size_t size() const { return orders_.size(); }
  std::optional<lob::Price> best(lob::Side s) const {
    std::optional<lob::Price> r;
    for (const Ord& o : orders_) {
      if (o.side != s) continue;
      if (!r || (s == lob::Side::Buy ? o.price > *r : o.price < *r)) r = o.price;
    }
    return r;
  }

 private:
  // Is resting order `a` a better match for a taker of side `taker` than `b`?
  static bool better(lob::Side taker, const Ord& a, const Ord& b) {
    if (a.price != b.price) return taker == lob::Side::Buy ? a.price < b.price : a.price > b.price;
    return a.seq < b.seq;
  }
  int find(lob::OrderId id) const {
    for (int i = 0; i < static_cast<int>(orders_.size()); ++i) if (orders_[i].id == id) return i;
    return -1;
  }
  std::vector<Ord> orders_;
  std::uint64_t seq_ = 0;
};
