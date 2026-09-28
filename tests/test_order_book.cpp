#include <cstdint>
#include <vector>
#include <limits>

#include "lob/order_book.hpp"
#include "lob/page_allocator.hpp"
#include "lob/price_levels.hpp"
#include "testing.hpp"

using namespace lob;

namespace {
struct Collector {
  std::vector<Trade> v;
  void operator()(const Trade& t) { v.push_back(t); }
};
}  // namespace

TEST(limit_rests_when_not_crossing) {
  OrderBook b(64); Collector c;
  CHECK(b.add(1, Side::Buy, OrderType::Limit, 100, 10, 1, c) == AddResult::Rested);
  CHECK(b.add(2, Side::Sell, OrderType::Limit, 105, 10, 2, c) == AddResult::Rested);
  CHECK(c.v.empty());
  CHECK(*b.best_bid() == 100);
  CHECK(*b.best_ask() == 105);
}

TEST(price_then_time_priority) {
  OrderBook b(64); Collector c;
  b.add(1, Side::Buy, OrderType::Limit, 100, 10, 1, c);
  b.add(2, Side::Buy, OrderType::Limit, 100, 10, 2, c);
  b.add(3, Side::Buy, OrderType::Limit, 101, 5, 3, c);
  CHECK(b.add(4, Side::Sell, OrderType::Limit, 100, 12, 4, c) == AddResult::Filled);
  // best price first (101, order 3), then oldest at 100 (order 1)
  CHECK(c.v.size() == 2);
  CHECK((c.v[0] == Trade{4, 3, 101, 5, 4}));
  CHECK((c.v[1] == Trade{4, 1, 100, 7, 4}));
  CHECK(b.volume_at(Side::Buy, 100) == 13);  // 3 left of #1, 10 of #2
  CHECK(b.order_count() == 2);
}

TEST(trade_price_is_maker_price) {
  OrderBook b(64); Collector c;
  b.add(1, Side::Sell, OrderType::Limit, 100, 5, 1, c);
  b.add(2, Side::Buy, OrderType::Limit, 110, 5, 2, c);
  CHECK(c.v.size() == 1 && c.v[0].price == 100);
}

TEST(partial_fill_rests_remainder) {
  OrderBook b(64); Collector c;
  b.add(1, Side::Sell, OrderType::Limit, 100, 4, 1, c);
  CHECK(b.add(2, Side::Buy, OrderType::Limit, 100, 10, 2, c) == AddResult::PartiallyRested);
  CHECK(b.volume_at(Side::Buy, 100) == 6);
  CHECK(!b.best_ask().has_value());
}

TEST(market_order_sweeps_levels_and_discards_remainder) {
  OrderBook b(64); Collector c;
  b.add(1, Side::Sell, OrderType::Limit, 100, 5, 1, c);
  b.add(2, Side::Sell, OrderType::Limit, 101, 5, 2, c);
  CHECK(b.add(3, Side::Buy, OrderType::Market, 0, 20, 3, c) == AddResult::Discarded);
  CHECK(c.v.size() == 2);
  CHECK(b.order_count() == 0);
  CHECK(!b.contains(3));  // market orders never rest
}

TEST(market_order_on_empty_book) {
  OrderBook b(64);
  CHECK(b.add(1, Side::Sell, OrderType::Market, 0, 5, 1, NullSink{}) == AddResult::Discarded);
}

TEST(cancel_removes_and_prunes_level) {
  OrderBook b(64);
  b.add(1, Side::Buy, OrderType::Limit, 100, 10, 1, NullSink{});
  b.add(2, Side::Buy, OrderType::Limit, 100, 10, 2, NullSink{});
  CHECK(b.cancel(1));
  CHECK(b.volume_at(Side::Buy, 100) == 10);
  CHECK(b.cancel(2));
  CHECK(!b.best_bid().has_value());
  CHECK(b.level_count(Side::Buy) == 0);
  CHECK(!b.cancel(2));   // already gone
  CHECK(!b.cancel(999)); // never existed
}

TEST(cancel_middle_of_queue_preserves_fifo) {
  OrderBook b(64); Collector c;
  b.add(1, Side::Sell, OrderType::Limit, 100, 1, 1, c);
  b.add(2, Side::Sell, OrderType::Limit, 100, 1, 2, c);
  b.add(3, Side::Sell, OrderType::Limit, 100, 1, 3, c);
  b.cancel(2);
  b.add(4, Side::Buy, OrderType::Market, 0, 2, 4, c);
  CHECK(c.v.size() == 2 && c.v[0].maker == 1 && c.v[1].maker == 3);
}

TEST(rejects_duplicate_id_and_zero_qty) {
  OrderBook b(64);
  b.add(1, Side::Buy, OrderType::Limit, 100, 10, 1, NullSink{});
  CHECK(b.add(1, Side::Buy, OrderType::Limit, 99, 10, 2, NullSink{}) == AddResult::Rejected);
  CHECK(b.add(2, Side::Buy, OrderType::Limit, 99, 0, 3, NullSink{}) == AddResult::Rejected);
}

TEST(flat_order_index_shifts_wrapped_collision_cluster_on_erase) {
  // A table of 8 slots (capacity 7). Pick four ids whose home is the last slot (same
  // Fibonacci hash as the index: top 3 bits of id * 2^64/phi), so the cluster wraps to slot 0.
  std::vector<OrderId> ids;
  for (OrderId id = 1; ids.size() < 4; ++id)
    if (((id * 0x9E3779B97F4A7C15ULL) >> 61) == 7) ids.push_back(id);
  FlatOrderIndex index(7);
  CHECK(index.insert(ids[0], 1));
  CHECK(index.insert(ids[1], 2));  // wraps to slot 0
  CHECK(index.insert(ids[2], 3));  // slot 1
  CHECK(index.erase(ids[1]));      // backward shift must move ids[2] into the hole

  Handle handle = kInvalid;
  CHECK(index.find(ids[0], handle) && handle == 1);
  CHECK(index.find(ids[2], handle) && handle == 3);
  CHECK(!index.contains(ids[1]));
  CHECK(index.insert(ids[3], 4));
  CHECK(index.find(ids[3], handle) && handle == 4);
  CHECK(index.size() == 3);
}

TEST(flat_order_index_stays_fast_with_sequential_ids) {
  // Regression for the identity-hash clustering: churn sequential ids through a half-full table
  // for many wraps and check that probe runs stay short (the whole test must stay quick).
  FlatOrderIndex index(1u << 12);  // 8192 slots
  std::vector<OrderId> live;
  OrderId next = 1;
  for (int i = 0; i < 400'000; ++i) {
    if (live.size() < 3000 || i % 2 == 0) {  // then alternate: about 3000 live (37% load)
      CHECK(index.insert(next, static_cast<Handle>(next % 4096)));
      live.push_back(next++);
    } else {
      const std::size_t k = static_cast<std::size_t>(next * 7919 % live.size());
      CHECK(index.erase(live[k]));
      live[k] = live.back();
      live.pop_back();
    }
  }
  for (const OrderId id : live) CHECK(index.contains(id));
  CHECK(index.size() == live.size());
}

TEST(dense_price_levels_order_central_and_overflow_prices) {
  DensePriceLevels<std::less<Price>> asks(9'000, 2'001);  // pinned band: 9,000-11,000
  for (Price price : std::vector<Price>{std::numeric_limits<Price>::max(), 11'001, 11'000, 10'000,
                                        9'000, 8'999, std::numeric_limits<Price>::min()}) {
    asks.get_or_create(price).total_qty = 1;
  }

  CHECK(asks.size() == 7);
  CHECK(asks.best_price() == std::numeric_limits<Price>::min());
  std::vector<Price> ordered;
  asks.for_each_sorted([&](Price price, const PriceLevel&) { ordered.push_back(price); });
  CHECK((ordered == std::vector<Price>{std::numeric_limits<Price>::min(), 8'999, 9'000, 10'000,
                                       11'000, 11'001, std::numeric_limits<Price>::max()}));
  CHECK(asks.find(10'000) != nullptr);
  CHECK(asks.find(8'998) == nullptr);

  asks.erase(10'000);
  CHECK(asks.size() == 6);
  CHECK(asks.find(10'000) == nullptr);
  CHECK(asks.best_price() == std::numeric_limits<Price>::min());
  CHECK(asks.get_or_create(10'000).empty());
}

TEST(dense_price_levels_order_bids_descending) {
  DensePriceLevels<std::greater<Price>> bids(9'000, 2'001);
  for (Price price : std::vector<Price>{std::numeric_limits<Price>::min(), 8'999, 9'000, 10'000,
                                        11'000, 11'001, std::numeric_limits<Price>::max()}) {
    bids.get_or_create(price).total_qty = 1;
  }

  CHECK(bids.best_price() == std::numeric_limits<Price>::max());
  std::vector<Price> ordered;
  bids.for_each_sorted([&](Price price, const PriceLevel&) { ordered.push_back(price); });
  CHECK((ordered == std::vector<Price>{std::numeric_limits<Price>::max(), 11'001, 11'000, 10'000,
                                       9'000, 8'999, std::numeric_limits<Price>::min()}));
}

TEST(dense_price_levels_anchor_band_on_first_price) {
  // Default band follows the instrument: 101 slots centred on the first price (3'235'250).
  DensePriceLevels<std::less<Price>> asks(101);
  CHECK(asks.find(3'235'250) == nullptr && asks.best_price() == std::nullopt);
  for (Price price : std::vector<Price>{3'235'250, 3'235'200, 3'235'300, 3'235'199, 3'235'301, 42})
    asks.get_or_create(price).total_qty = 1;
  CHECK(asks.size() == 6 && asks.best_price() == 42);
  std::vector<Price> ordered;
  asks.for_each_sorted([&](Price price, const PriceLevel&) { ordered.push_back(price); });
  CHECK((ordered == std::vector<Price>{42, 3'235'199, 3'235'200, 3'235'250, 3'235'300, 3'235'301}));
  asks.erase(42);
  CHECK(asks.best_price() == 3'235'199);
  asks.erase(3'235'199);
  CHECK(asks.best_price() == 3'235'200);  // the band's lowest slot

  // Anchoring at the extremes of Price clamps the band instead of overflowing.
  DensePriceLevels<std::greater<Price>> top(101), bottom(101);
  top.get_or_create(std::numeric_limits<Price>::max()).total_qty = 1;
  bottom.get_or_create(std::numeric_limits<Price>::min()).total_qty = 1;
  CHECK(top.best_price() == std::numeric_limits<Price>::max());
  CHECK(bottom.best_price() == std::numeric_limits<Price>::min());
}

TEST(dense_order_book_matches_tree_at_real_price_scale) {
  OrderBook tree(64);
  DenseOrderBook dense(64);
  std::vector<Trade> a, b;
  for (OrderId id = 1; id <= 40; ++id) {
    const Side side = id % 2 ? Side::Buy : Side::Sell;
    const Price price = side == Side::Buy ? 3'235'000 - Price(id % 7) * 100
                                          : 3'235'100 + Price(id % 5) * 100;
    tree.add(id, side, OrderType::Limit, price, Qty(id), id, [&](const Trade& t) { a.push_back(t); });
    dense.add(id, side, OrderType::Limit, price, Qty(id), id, [&](const Trade& t) { b.push_back(t); });
  }
  tree.add(99, Side::Buy, OrderType::Market, 0, 200, 99, [&](const Trade& t) { a.push_back(t); });
  dense.add(99, Side::Buy, OrderType::Market, 0, 200, 99, [&](const Trade& t) { b.push_back(t); });
  CHECK(a == b && !a.empty());
  CHECK(tree.checksum() == dense.checksum());
}

TEST(dense_order_book_matches_tree_through_overflow_prices) {
  OrderBook tree(32);
  DenseOrderBook dense(32);
  std::vector<Trade> tree_trades;
  std::vector<Trade> dense_trades;
  const std::vector<Price> prices{std::numeric_limits<Price>::min(), 8'999, 9'000, 10'000,
                                  11'000, 11'001, std::numeric_limits<Price>::max()};

  for (std::size_t i = 0; i < prices.size(); ++i) {
    const auto tree_result = tree.add(i + 1, Side::Sell, OrderType::Limit, prices[i], 1, i,
                                      [&](const Trade& trade) { tree_trades.push_back(trade); });
    const auto dense_result = dense.add(i + 1, Side::Sell, OrderType::Limit, prices[i], 1, i,
                                        [&](const Trade& trade) { dense_trades.push_back(trade); });
    CHECK(tree_result == dense_result);
    CHECK(tree.checksum() == dense.checksum());
    CHECK(tree.best_ask() == dense.best_ask());
  }

  const auto tree_result = tree.add(99, Side::Buy, OrderType::Market, 0, 10, 99,
                                   [&](const Trade& trade) { tree_trades.push_back(trade); });
  const auto dense_result = dense.add(99, Side::Buy, OrderType::Market, 0, 10, 99,
                                      [&](const Trade& trade) { dense_trades.push_back(trade); });
  CHECK(tree_result == dense_result);
  CHECK(tree_trades == dense_trades);
  CHECK(tree.order_count() == dense.order_count());
  CHECK(tree.checksum() == dense.checksum());
}

TEST(sorted_price_levels_keep_best_at_back_for_bids_and_asks) {
  SortedPriceLevels<std::less<Price>> asks;
  SortedPriceLevels<std::greater<Price>> bids;
  for (Price price : std::vector<Price>{11, 9, 12, 10}) {
    asks.get_or_create(price).total_qty = 1;
    bids.get_or_create(price).total_qty = 1;
  }
  CHECK(asks.best_price() == 9);
  CHECK(bids.best_price() == 12);

  std::vector<Price> ask_order;
  asks.for_each_sorted([&](Price price, const PriceLevel&) { ask_order.push_back(price); });
  CHECK((ask_order == std::vector<Price>{9, 10, 11, 12}));
  std::vector<Price> bid_order;
  bids.for_each_sorted([&](Price price, const PriceLevel&) { bid_order.push_back(price); });
  CHECK((bid_order == std::vector<Price>{12, 11, 10, 9}));

  asks.erase(10);
  CHECK(asks.find(10) == nullptr && asks.size() == 3);
}

TEST(sorted_order_book_matches_tree_fills_and_cancels) {
  OrderBook tree(32);
  SortedOrderBook sorted(32);
  std::vector<Trade> tree_trades;
  std::vector<Trade> sorted_trades;
  for (Price price : std::vector<Price>{9'999, 10'001, 9'998, 10'002, 10'000}) {
    const auto tree_result = tree.add(static_cast<OrderId>(price), Side::Sell, OrderType::Limit,
                                      price, 2, 1,
                                      [&](const Trade& trade) { tree_trades.push_back(trade); });
    const auto sorted_result = sorted.add(
        static_cast<OrderId>(price), Side::Sell, OrderType::Limit, price, 2, 1,
        [&](const Trade& trade) { sorted_trades.push_back(trade); });
    CHECK(tree_result == sorted_result);
    CHECK(tree.best_ask() == sorted.best_ask());
    CHECK(tree.checksum() == sorted.checksum());
  }

  CHECK(tree.cancel(10'001) == sorted.cancel(10'001));
  const auto tree_result = tree.add(99, Side::Buy, OrderType::Market, 0, 20, 2,
                                   [&](const Trade& trade) { tree_trades.push_back(trade); });
  const auto sorted_result = sorted.add(
      99, Side::Buy, OrderType::Market, 0, 20, 2,
      [&](const Trade& trade) { sorted_trades.push_back(trade); });
  CHECK(tree_result == sorted_result);
  CHECK(tree_trades == sorted_trades);
  CHECK(tree.checksum() == sorted.checksum());
}

TEST(pool_slots_are_reused) {
  OrderBook b(4);
  for (OrderId i = 1; i <= 4; ++i) b.add(i, Side::Buy, OrderType::Limit, 100 - i, 1, i, NullSink{});
  CHECK(b.add(5, Side::Buy, OrderType::Limit, 1, 1, 5, NullSink{}) == AddResult::Rejected);  // full
  b.cancel(2);
  CHECK(b.add(5, Side::Buy, OrderType::Limit, 1, 1, 5, NullSink{}) == AddResult::Rested);
}

TEST(crossing_limit_orders_trade_at_resting_price) {
  OrderBook b(64); Collector c;
  b.add(1, Side::Buy, OrderType::Limit, 105, 4, 1, c);
  CHECK(b.add(2, Side::Sell, OrderType::Limit, 100, 4, 2, c) == AddResult::Filled);
  CHECK(c.v.size() == 1);
  CHECK((c.v[0] == Trade{2, 1, 105, 4, 2}));
  CHECK(b.order_count() == 0);
}

TEST(market_order_on_one_sided_book_discards_remainder) {
  OrderBook b(64); Collector c;
  b.add(1, Side::Sell, OrderType::Limit, 100, 3, 1, c);
  CHECK(b.add(2, Side::Buy, OrderType::Market, 0, 8, 2, c) == AddResult::Discarded);
  CHECK(c.v.size() == 1 && c.v[0].maker == 1 && c.v[0].qty == 3);
  CHECK(b.order_count() == 0);
}

TEST(cancel_maker_after_partial_fill) {
  OrderBook b(64); Collector c;
  b.add(1, Side::Sell, OrderType::Limit, 100, 10, 1, c);
  CHECK(b.add(2, Side::Buy, OrderType::Limit, 100, 4, 2, c) == AddResult::Filled);
  CHECK(b.order_count() == 1 && b.volume_at(Side::Sell, 100) == 6);
  CHECK(b.cancel(1));
  CHECK(b.order_count() == 0 && b.level_count(Side::Sell) == 0);
}

TEST(large_quantity_partially_fills_without_overflow) {
  OrderBook b(64); Collector c;
  b.add(1, Side::Sell, OrderType::Limit, 100, 7, 1, c);
  const Qty large_qty = std::numeric_limits<Qty>::max();
  CHECK(b.add(2, Side::Buy, OrderType::Limit, 100, large_qty, 2, c) ==
        AddResult::PartiallyRested);
  CHECK(c.v.size() == 1 && c.v[0].qty == 7);
  CHECK(b.volume_at(Side::Buy, 100) == large_qty - 7);
  CHECK(b.order_count() == 1);
}

TEST(filled_order_id_can_be_reused) {
  OrderBook b(64);
  b.add(1, Side::Sell, OrderType::Limit, 100, 2, 1, NullSink{});
  CHECK(b.add(2, Side::Buy, OrderType::Market, 0, 2, 2, NullSink{}) == AddResult::Filled);
  CHECK(b.add(1, Side::Buy, OrderType::Limit, 99, 3, 3, NullSink{}) == AddResult::Rested);
  CHECK(b.contains(1) && b.order_count() == 1);
}

// ---- Phase 5(c): modify / reduce ---------------------------------------------------------

TEST(reduce_keeps_queue_position) {
  OrderBook b(64); Collector c;
  b.add(1, Side::Sell, OrderType::Limit, 100, 10, 1, c);
  b.add(2, Side::Sell, OrderType::Limit, 100, 10, 2, c);
  CHECK(b.reduce(1, 7));
  CHECK(b.volume_at(Side::Sell, 100) == 13);
  b.add(3, Side::Buy, OrderType::Market, 0, 4, 3, c);
  CHECK(c.v.size() == 2);
  CHECK((c.v[0] == Trade{3, 1, 100, 3, 3}));  // still first in the queue, with 3 left
  CHECK((c.v[1] == Trade{3, 2, 100, 1, 3}));
}

TEST(reduce_by_all_or_more_removes_order_and_level) {
  OrderBook b(64);
  b.add(1, Side::Buy, OrderType::Limit, 100, 5, 1, NullSink{});
  CHECK(b.reduce(1, 9));
  CHECK(!b.contains(1) && b.order_count() == 0 && b.level_count(Side::Buy) == 0);
  CHECK(!b.reduce(1, 1));   // already gone
  CHECK(!b.reduce(99, 1));  // never existed
}

TEST(modify_lower_qty_same_price_keeps_priority) {
  OrderBook b(64); Collector c;
  b.add(1, Side::Buy, OrderType::Limit, 100, 10, 1, c);
  b.add(2, Side::Buy, OrderType::Limit, 100, 10, 2, c);
  CHECK(b.modify(1, 100, 4, 3, c) == ModifyResult::Amended);
  CHECK(b.modify(1, 100, 4, 4, c) == ModifyResult::Amended);  // unchanged: no-op, still first
  CHECK(b.volume_at(Side::Buy, 100) == 14);
  b.add(3, Side::Sell, OrderType::Market, 0, 5, 5, c);
  CHECK(c.v.size() == 2 && c.v[0].maker == 1 && c.v[0].qty == 4 && c.v[1].maker == 2);
}

TEST(modify_higher_qty_loses_priority) {
  OrderBook b(64); Collector c;
  b.add(1, Side::Buy, OrderType::Limit, 100, 10, 1, c);
  b.add(2, Side::Buy, OrderType::Limit, 100, 10, 2, c);
  CHECK(b.modify(1, 100, 11, 3, c) == ModifyResult::Rested);
  CHECK(b.volume_at(Side::Buy, 100) == 21);
  b.add(3, Side::Sell, OrderType::Market, 0, 12, 4, c);
  CHECK(c.v.size() == 2 && c.v[0].maker == 2 && c.v[0].qty == 10 && c.v[1].maker == 1);
}

TEST(modify_price_moves_level_and_loses_priority) {
  OrderBook b(64); Collector c;
  b.add(1, Side::Sell, OrderType::Limit, 101, 5, 1, c);
  b.add(2, Side::Sell, OrderType::Limit, 100, 5, 2, c);
  CHECK(b.modify(1, 100, 3, 3, c) == ModifyResult::Rested);  // lower qty, but new price
  CHECK(b.level_count(Side::Sell) == 1 && b.volume_at(Side::Sell, 100) == 8);
  CHECK(b.volume_at(Side::Sell, 101) == 0);
  b.add(3, Side::Buy, OrderType::Market, 0, 6, 4, c);
  CHECK(c.v.size() == 2 && c.v[0].maker == 2 && c.v[1].maker == 1);
}

TEST(modify_to_crossing_price_trades_at_maker_price) {
  OrderBook b(64); Collector c;
  b.add(1, Side::Sell, OrderType::Limit, 100, 4, 1, c);
  b.add(2, Side::Buy, OrderType::Limit, 98, 10, 2, c);
  CHECK(b.modify(2, 101, 10, 3, c) == ModifyResult::PartiallyRested);
  CHECK(c.v.size() == 1);
  CHECK((c.v[0] == Trade{2, 1, 100, 4, 3}));  // modified order is the taker
  CHECK(b.volume_at(Side::Buy, 101) == 6 && b.volume_at(Side::Buy, 98) == 0);
  CHECK(!b.best_ask().has_value());

  b.add(3, Side::Sell, OrderType::Limit, 105, 6, 4, c);
  CHECK(b.modify(2, 105, 6, 5, c) == ModifyResult::Filled);
  CHECK(b.order_count() == 0);
}

TEST(modify_misses_and_rejects) {
  OrderBook b(64); Collector c;
  CHECK(b.modify(1, 100, 5, 1, c) == ModifyResult::NotFound);
  b.add(1, Side::Buy, OrderType::Limit, 100, 5, 2, c);
  CHECK(b.modify(1, 100, 0, 3, c) == ModifyResult::Rejected);
  CHECK(b.contains(1) && b.volume_at(Side::Buy, 100) == 5);  // unchanged
}

TEST(modify_requeue_works_with_full_pool) {
  OrderBook b(2);
  b.add(1, Side::Buy, OrderType::Limit, 100, 5, 1, NullSink{});
  b.add(2, Side::Buy, OrderType::Limit, 99, 5, 2, NullSink{});
  CHECK(b.modify(2, 98, 7, 3, NullSink{}) == ModifyResult::Rested);
  CHECK(b.order_count() == 2 && b.volume_at(Side::Buy, 98) == 7);
}

TEST(modify_and_reduce_match_across_variants) {
  OrderBook tree(64);
  DenseOrderBook dense(64);
  SortedOrderBook sorted(64);
  HotColdOrderBook hotcold(64);
  const auto run = [](auto& book) {
    for (OrderId id = 1; id <= 12; ++id)
      book.add(id, id % 2 ? Side::Buy : Side::Sell, OrderType::Limit,
               id % 2 ? 100 - Price(id % 4) : 101 + Price(id % 3), Qty(id + 2), id, NullSink{});
    book.reduce(3, 2);
    book.modify(5, 101, 4, 20, NullSink{});  // crosses
    book.modify(4, 101, 3, 21, NullSink{});
    book.reduce(8, 100);
    return book.checksum();
  };
  const auto expected = run(tree);
  CHECK(run(dense) == expected);
  CHECK(run(sorted) == expected);
  CHECK(run(hotcold) == expected);
}

TEST(huge_page_allocator_is_aligned_and_value_initializes) {
  std::vector<std::uint64_t, HugePageAllocator<std::uint64_t>> huge(1000);
  CHECK(reinterpret_cast<std::uintptr_t>(huge.data()) % kHugePageBytes == 0);
  huge[999] = 7;
  CHECK(huge[999] == 7 && huge[0] == 0);
  huge.resize(600'000);  // reallocation across several huge pages keeps contents
  CHECK(huge[999] == 7 && huge[599'999] == 0);
  CHECK(reinterpret_cast<std::uintptr_t>(huge.data()) % kHugePageBytes == 0);
}

TEST(huge_page_book_matches_default_book) {
  OrderBook plain(1024);
  HugePageOrderBook huge(1024);
  for (OrderId id = 1; id <= 200; ++id) {
    const Side side = id % 2 ? Side::Buy : Side::Sell;
    const Price price = side == Side::Buy ? 100 - Price(id % 7) : 99 + Price(id % 5);
    plain.add(id, side, OrderType::Limit, price, Qty(id % 9 + 1), id, NullSink{});
    huge.add(id, side, OrderType::Limit, price, Qty(id % 9 + 1), id, NullSink{});
    if (id % 3 == 0) {
      plain.cancel(id - 1);
      huge.cancel(id - 1);
    }
  }
  CHECK(plain.checksum() == huge.checksum());
  CHECK(plain.order_count() == huge.order_count());
}

TEST(prefault_pool_does_not_change_state) {
  OrderBook b(64); Collector c;
  b.add(1, Side::Buy, OrderType::Limit, 100, 5, 1, c);
  b.add(2, Side::Sell, OrderType::Limit, 101, 3, 2, c);
  b.prefault_pool();
  CHECK(b.order_count() == 2);
  CHECK(b.volume_at(Side::Buy, 100) == 5);
  CHECK(b.volume_at(Side::Sell, 101) == 3);
  CHECK(b.best_bid().has_value() && *b.best_bid() == 100);
  CHECK(b.best_ask().has_value() && *b.best_ask() == 101);
}
