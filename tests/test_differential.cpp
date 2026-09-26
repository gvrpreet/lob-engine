// Differential test: optimised engine vs. naive oracle on random operation streams.
// THIS is the safety net that lets you optimise aggressively in phases 2-6.
#include <random>
#include <vector>

#include "lob/order_book.hpp"
#include "reference_book.hpp"
#include "testing.hpp"

using namespace lob;

template <class Book = OrderBook>
static void run_differential(std::uint64_t seed, int ops, Price price_base = 90) {
  Book book(1u << 16);
  RefBook ref;
  std::mt19937_64 rng(seed);
  std::vector<OrderId> ids;
  OrderId next = 1;

  for (int i = 0; i < ops; ++i) {
    const auto r = static_cast<unsigned>(rng() % 100);
    if (r < 25 && !ids.empty()) {
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
