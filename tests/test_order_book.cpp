#include <vector>
#include <limits>

#include "lob/order_book.hpp"
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
