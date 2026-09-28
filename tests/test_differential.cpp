// Differential test: optimised engine vs. naive oracle on random operation streams.
// THIS is the safety net that lets you optimise aggressively in phases 2-6.
#include <cstdio>
#include <random>
#include <string>
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
      std::string why;
      CHECK(book.validate(&why));
      if (!why.empty()) std::fprintf(stderr, "  validate at op %d: %s\n", i, why.c_str());
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
TEST(tree_differential_seed_1) { run_differential<TreeOrderBook>(1, 100'000, 9'990); }
TEST(sorted_differential_seed_1) { run_differential<SortedOrderBook>(1, 100'000, 9'990); }

// A drifting market (random walk far beyond the dense band's +/-1000 ticks) so the dense book
// re-centres many times; every op's result and trades, and the book checksum, must match the tree.
template <class Book>
static void run_drift_vs_tree(std::uint64_t seed, int ops) {
  TreeOrderBook tree(1u << 16);
  Book other(1u << 16);
  std::mt19937_64 rng(seed);
  std::vector<OrderId> ids;
  OrderId next = 1;
  Price mid = 1'000'000;
  for (int i = 0; i < ops; ++i) {
    if (i % 50 == 0) mid += static_cast<Price>(rng() % 601) - 300;
    const auto r = static_cast<unsigned>(rng() % 100);
    std::vector<Trade> a, b;
    const auto ta = [&](const Trade& t) { a.push_back(t); };
    const auto tb = [&](const Trade& t) { b.push_back(t); };
    if (r < 25 && !ids.empty()) {
      const OrderId id = ids[rng() % ids.size()];
      CHECK(tree.cancel(id) == other.cancel(id));
    } else if (r < 35 && !ids.empty()) {
      const OrderId id = ids[rng() % ids.size()];
      const Price price = mid + static_cast<Price>(rng() % 41) - 20;
      const Qty qty = 1 + static_cast<Qty>(rng() % 50);
      CHECK(tree.modify(id, price, qty, i, ta) == other.modify(id, price, qty, i, tb));
    } else {
      const OrderId id = next++;
      const Side side = (rng() & 1) ? Side::Buy : Side::Sell;
      const OrderType type = r < 45 ? OrderType::Market : OrderType::Limit;
      const Price price = mid + static_cast<Price>(rng() % 41) - 20;
      const Qty qty = 1 + static_cast<Qty>(rng() % 60);
      CHECK(tree.add(id, side, type, price, qty, i, ta) == other.add(id, side, type, price, qty, i, tb));
      ids.push_back(id);
    }
    CHECK(a == b);
    if (i % 256 == 0) CHECK(tree.checksum() == other.checksum() && tree.validate() && other.validate());
  }
  CHECK(tree.checksum() == other.checksum() && tree.order_count() == other.order_count());
}

TEST(dense_recentring_matches_tree_in_drifting_market) {
  run_drift_vs_tree<DenseOrderBook>(11, 60'000);
  run_drift_vs_tree<DenseOrderBook>(12, 60'000);
  run_drift_vs_tree<SortedOrderBook>(13, 20'000);
}

// Phase 5(c): modify/reduce on every storage variant.
TEST(amend_differential_seed_1) { run_differential<OrderBook>(1, 100'000, 90, true); }
TEST(amend_differential_seed_2) { run_differential<OrderBook>(2, 100'000, 90, true); }
TEST(amend_differential_seed_3) { run_differential<OrderBook>(0xC0FFEE, 100'000, 90, true); }
TEST(amend_differential_variants) {
  run_differential<NaiveOrderBook>(4, 30'000, 90, true);
  run_differential<OrderBook48>(5, 30'000, 90, true);
  run_differential<OrderBook64>(6, 30'000, 90, true);
  run_differential<HotColdOrderBook>(7, 30'000, 90, true);
  run_differential<TreeOrderBook>(8, 30'000, 9'990, true);
  run_differential<SortedOrderBook>(9, 30'000, 9'990, true);
  run_differential<HugePageOrderBook>(10, 30'000, 90, true);
}
