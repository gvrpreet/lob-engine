// Differential test: optimised engine vs. naive oracle on random operation streams.
// THIS is the safety net that lets you optimise aggressively in phases 2-6.
#include <random>
#include <vector>

#include "lob/order_book.hpp"
#include "reference_book.hpp"
#include "testing.hpp"

using namespace lob;

// `amend` adds modify and reduce to the mix. Without it the random stream is exactly the
// original one, so the plain tests keep their historical coverage.
template <class Book = OrderBook>
static void run_differential(std::uint64_t seed, int ops, Price price_base = 90,
                             bool amend = false) {
  Book book(1u << 16);
  RefBook ref;
  std::mt19937_64 rng(seed);
  std::vector<OrderId> ids;
  OrderId next = 1;

  for (int i = 0; i < ops; ++i) {
    const auto r = static_cast<unsigned>(rng() % 100);
    if (amend && r >= 85 && !ids.empty()) {
      const OrderId id = ids[rng() % ids.size()];  // may already be gone: exercises the miss path
      if (r < 93) {
        const Price price = price_base + static_cast<Price>(rng() % 21);
        const Qty qty = static_cast<Qty>(rng() % 60);  // includes 0 => reject path
        std::vector<Trade> a, b;
        const auto ma = book.modify(id, price, qty, i, [&](const Trade& t) { a.push_back(t); });
        const auto mb = ref.modify(id, price, qty, i, b);
        CHECK(ma == mb);
        CHECK(a == b);
      } else {
        const Qty by = static_cast<Qty>(rng() % 40);
        CHECK(book.reduce(id, by) == ref.reduce(id, by));
      }
    } else if (r < 25 && !ids.empty()) {
      const OrderId id = ids[rng() % ids.size()];  // may already be filled/cancelled: on purpose
      CHECK(book.cancel(id) == ref.cancel(id));
    } else {
      const OrderId id = next++;
      const Side side = (rng() & 1) ? Side::Buy : Side::Sell;
      const OrderType type = r < 35 ? OrderType::Market : OrderType::Limit;
      const Price price = price_base + static_cast<Price>(rng() % 21);
      const Qty qty = static_cast<Qty>(rng() % 60);  // includes 0 => reject path
      std::vector<Trade> a, b;
      const auto ra = book.add(id, side, type, price, qty, i, [&](const Trade& t) { a.push_back(t); });
      const auto rb = ref.add(id, side, type, price, qty, i, b);
      CHECK(ra == rb);
      CHECK(a == b);
      ids.push_back(id);
    }
    if (i % 512 == 0) {
      CHECK(book.order_count() == ref.size());
      CHECK(book.best_bid() == ref.best(Side::Buy));
      CHECK(book.best_ask() == ref.best(Side::Sell));
    }
  }
  CHECK(book.order_count() == ref.size());
}

TEST(differential_seed_1) { run_differential<OrderBook>(1, 100'000); }
TEST(differential_seed_2) { run_differential<OrderBook>(2, 100'000); }
TEST(differential_seed_3) { run_differential<OrderBook>(0xC0FFEE, 100'000); }
TEST(dense_differential_seed_1) { run_differential<DenseOrderBook>(1, 100'000, 9'990); }
TEST(sorted_differential_seed_1) { run_differential<SortedOrderBook>(1, 100'000, 9'990); }

// Phase 5(c): modify/reduce on every storage variant.
TEST(amend_differential_seed_1) { run_differential<OrderBook>(1, 100'000, 90, true); }
TEST(amend_differential_seed_2) { run_differential<OrderBook>(2, 100'000, 90, true); }
TEST(amend_differential_seed_3) { run_differential<OrderBook>(0xC0FFEE, 100'000, 90, true); }
TEST(amend_differential_variants) {
  run_differential<NaiveOrderBook>(4, 30'000, 90, true);
  run_differential<OrderBook48>(5, 30'000, 90, true);
  run_differential<OrderBook64>(6, 30'000, 90, true);
  run_differential<HotColdOrderBook>(7, 30'000, 90, true);
  run_differential<DenseOrderBook>(8, 30'000, 9'990, true);
  run_differential<SortedOrderBook>(9, 30'000, 9'990, true);
  run_differential<HugePageOrderBook>(10, 30'000, 90, true);
}
