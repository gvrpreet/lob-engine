#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>

#include "lob/flat_order_index.hpp"
#include "lob/order.hpp"
#include "lob/order_pool.hpp"
#include "lob/price_level.hpp"
#include "lob/price_levels.hpp"
#include "lob/types.hpp"

namespace lob {

struct NullSink {
  void operator()(const Trade&) const noexcept {}
};

// Price-time priority limit order book. Single-threaded by design.
//
// PHASE 1 BASELINE (see docs/00-roadmap.md):
//   price levels : TreePriceLevels     -> dense bitmap experiment available (phase 3b)
//   id lookup    : FlatOrderIndex      -> open-addressing table (phase 3.1)
//   order storage: OrderPool (slab)    -> already cache-aware (phase 2)
template <class Pool, template <class> class LevelIndex = TreePriceLevels>
class BasicOrderBook {
 public:
  using Bids = LevelIndex<std::greater<Price>>;
  using Asks = LevelIndex<std::less<Price>>;

  explicit BasicOrderBook(std::size_t capacity = 1u << 20)
      : pool_(capacity), index_(capacity) {}

  void prefault_pool() noexcept { pool_.prefault(); }
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
    pool_.set_order(h, id, price, qty, side);
    if (!index_.insert(id, h)) {
      pool_.release(h);
      return AddResult::Rejected;
    }
    if (side == Side::Buy) bids_.get_or_create(price).push_back(pool_, h);
    else asks_.get_or_create(price).push_back(pool_, h);
    return qty == original ? AddResult::Rested : AddResult::PartiallyRested;
  }

  // Remove a resting order. Returns false if the id is not resting.
  bool cancel(OrderId id) {
    Handle h;
    if (!index_.find(id, h)) return false;
    if (pool_.side(h) == Side::Buy) unlink_and_prune(bids_, h);
    else unlink_and_prune(asks_, h);
    index_.erase(id);
    pool_.release(h);
    return true;
  }

  // ---- read-only queries --------------------------------------------------
  std::optional<Price> best_bid() const {
    return bids_.best_price();
  }
  std::optional<Price> best_ask() const {
    return asks_.best_price();
  }
  std::size_t order_count() const noexcept { return index_.size(); }
  std::size_t level_count(Side s) const noexcept {
    return s == Side::Buy ? bids_.size() : asks_.size();
  }
  bool contains(OrderId id) const { return index_.contains(id); }

  Qty volume_at(Side s, Price p) const {
    if (s == Side::Buy) {
      const PriceLevel* level = bids_.find(p);
      return level == nullptr ? 0 : level->total_qty;
    }
    const PriceLevel* level = asks_.find(p);
    return level == nullptr ? 0 : level->total_qty;
  }

  // Order-sensitive hash of the entire book state. Used to prove that two
  // replays (or an optimized vs baseline implementation) are identical.
  std::uint64_t checksum() const noexcept {
    std::uint64_t h = 1469598103934665603ULL;
    const auto mix = [&h](std::uint64_t value) {
      h ^= value;
      h *= 1099511628211ULL;
    };
    const auto walk = [&](const auto& book, std::uint64_t tag) {
      book.for_each_sorted([&](Price price, const PriceLevel& level) {
        mix(tag);
        mix(static_cast<std::uint64_t>(price));
        for (Handle handle = level.head; handle != kInvalid; handle = pool_.next(handle)) {
          mix(pool_.id(handle));
          mix(pool_.qty(handle));
        }
      });
    };
    walk(bids_, 1);
    walk(asks_, 2);
    return h;
  }

 private:
  static bool crosses(Side taker, Price limit, Price level) noexcept {
    return taker == Side::Buy ? limit >= level : limit <= level;
  }

  template <class Book, class Sink>
  Qty match(Book& opposite, OrderId taker, Side side, OrderType type, Price limit, Qty qty,
            Timestamp ts, Sink& sink) {
    while (qty > 0 && !opposite.empty()) {
      Price level_price;
      PriceLevel* level_ptr = opposite.best(level_price);
      if (level_ptr == nullptr) break;
      if (type == OrderType::Limit && !crosses(side, limit, level_price)) break;

      PriceLevel& level = *level_ptr;
      while (qty > 0 && !level.empty()) {
        const Handle h = level.head;  // oldest order = time priority
        const Qty maker_qty = pool_.qty(h);
        const Qty fill = std::min(qty, maker_qty);
        pool_.set_qty(h, maker_qty - fill);
        level.total_qty -= fill;
        qty -= fill;
        sink(Trade{taker, pool_.id(h), level_price, fill, ts});
        if (maker_qty == fill) {
          level.unlink(pool_, h);
          index_.erase(pool_.id(h));
          pool_.release(h);
        }
      }
      if (level.empty()) opposite.erase(level_price);
    }
    return qty;
  }

  template <class Book>
  void unlink_and_prune(Book& book, Handle h) {
    const Price price = pool_.price(h);
    PriceLevel* level = book.find(price);
    if (level == nullptr) return;
    level->unlink(pool_, h);
    if (level->empty()) book.erase(price);
  }

  Pool pool_;
  Bids bids_;
  Asks asks_;
  FlatOrderIndex index_;
};

using OrderBook = BasicOrderBook<OrderPool>;
using NaiveOrderBook = BasicOrderBook<HeapOrderPool>;
using OrderBook48 = BasicOrderBook<OrderPool48>;
using OrderBook64 = BasicOrderBook<OrderPool64>;
using HotColdOrderBook = BasicOrderBook<HotColdOrderPool>;
using DenseOrderBook = BasicOrderBook<OrderPool, DensePriceLevels>;
using SortedOrderBook = BasicOrderBook<OrderPool, SortedPriceLevels>;

}  // namespace lob
