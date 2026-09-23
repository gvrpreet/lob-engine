#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <unordered_map>

#include "lob/order.hpp"
#include "lob/order_pool.hpp"
#include "lob/price_level.hpp"
#include "lob/types.hpp"

namespace lob {

struct NullSink {
  void operator()(const Trade&) const noexcept {}
};

// Price-time priority limit order book. Single-threaded by design.
//
// PHASE 1 BASELINE (see docs/00-roadmap.md):
//   price levels : std::map            -> replace with flat ladder / bitmap (phase 3)
//   id lookup    : std::unordered_map  -> replace with open-addressing table  (phase 3)
//   order storage: OrderPool (slab)    -> already cache-aware (phase 2)
class OrderBook {
 public:
  using Bids = std::map<Price, PriceLevel, std::greater<Price>>;
  using Asks = std::map<Price, PriceLevel, std::less<Price>>;

  explicit OrderBook(std::size_t capacity = 1u << 20) : pool_(capacity) {
    index_.reserve(capacity);
  }

  // Submit a market or limit order. `sink(const Trade&)` is invoked per fill.
  // Market orders ignore `price`. Unfilled market remainder is discarded.
  // Limit orders are conservatively rejected when the pool is full.
  template <class Sink>
  AddResult add(OrderId id, Side side, OrderType type, Price price, Qty qty, Timestamp ts,
                Sink&& sink) {
    if (qty == 0 || index_.contains(id)) return AddResult::Rejected;
    if (type == OrderType::Limit && pool_.full()) return AddResult::Rejected;

    const Qty original = qty;
    qty = (side == Side::Buy) ? match(asks_, id, side, type, price, qty, ts, sink)
                              : match(bids_, id, side, type, price, qty, ts, sink);

    if (qty == 0) return AddResult::Filled;
    if (type == OrderType::Market) return AddResult::Discarded;

    const Handle h = pool_.allocate();  // cannot fail: checked above, and matching only frees
    Order& o = pool_[h];
    o.id = id;
    o.price = price;
    o.qty = qty;
    o.side = side;
    if (side == Side::Buy) bids_[price].push_back(pool_, h);
    else asks_[price].push_back(pool_, h);
    index_.emplace(id, h);
    return qty == original ? AddResult::Rested : AddResult::PartiallyRested;
  }

  // Remove a resting order. Returns false if the id is not resting.
  bool cancel(OrderId id) {
    const auto it = index_.find(id);
    if (it == index_.end()) return false;
    const Handle h = it->second;
    if (pool_[h].side == Side::Buy) unlink_and_prune(bids_, h);
    else unlink_and_prune(asks_, h);
    index_.erase(it);
    pool_.release(h);
    return true;
  }

  // ---- read-only queries --------------------------------------------------
  std::optional<Price> best_bid() const {
    return bids_.empty() ? std::nullopt : std::optional<Price>(bids_.begin()->first);
  }
  std::optional<Price> best_ask() const {
    return asks_.empty() ? std::nullopt : std::optional<Price>(asks_.begin()->first);
  }
  std::size_t order_count() const noexcept { return index_.size(); }
  std::size_t level_count(Side s) const noexcept {
    return s == Side::Buy ? bids_.size() : asks_.size();
  }
  bool contains(OrderId id) const { return index_.contains(id); }

  Qty volume_at(Side s, Price p) const {
    if (s == Side::Buy) {
      const auto it = bids_.find(p);
      return it == bids_.end() ? 0 : it->second.total_qty;
    }
    const auto it = asks_.find(p);
    return it == asks_.end() ? 0 : it->second.total_qty;
  }

  // Order-sensitive hash of the entire book state. Used to prove that two
  // replays (or an optimized vs baseline implementation) are identical.
  std::uint64_t checksum() const noexcept;

 private:
  static bool crosses(Side taker, Price limit, Price level) noexcept {
    return taker == Side::Buy ? limit >= level : limit <= level;
  }

  template <class Book, class Sink>
  Qty match(Book& opposite, OrderId taker, Side side, OrderType type, Price limit, Qty qty,
            Timestamp ts, Sink& sink) {
    while (qty > 0 && !opposite.empty()) {
      const auto lit = opposite.begin();  // best price on the opposite side
      const Price level_price = lit->first;
      if (type == OrderType::Limit && !crosses(side, limit, level_price)) break;

      PriceLevel& level = lit->second;
      while (qty > 0 && !level.empty()) {
        const Handle h = level.head;  // oldest order = time priority
        Order& maker = pool_[h];
        const Qty fill = std::min(qty, maker.qty);
        maker.qty -= fill;
        level.total_qty -= fill;
        qty -= fill;
        sink(Trade{taker, maker.id, level_price, fill, ts});
        if (maker.qty == 0) {
          level.unlink(pool_, h);
          index_.erase(maker.id);
          pool_.release(h);
        }
      }
      if (level.empty()) opposite.erase(lit);
    }
    return qty;
  }

  template <class Book>
  void unlink_and_prune(Book& book, Handle h) {
    const auto lit = book.find(pool_[h].price);
    lit->second.unlink(pool_, h);
    if (lit->second.empty()) book.erase(lit);
  }

  OrderPool pool_;
  Bids bids_;
  Asks asks_;
  std::unordered_map<OrderId, Handle> index_;
};

}  // namespace lob
