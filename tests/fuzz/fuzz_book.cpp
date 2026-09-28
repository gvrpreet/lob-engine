// libFuzzer harness (Phase 7). The input is read as a stream of order-book operations (3 bytes
// each). Every operation runs on the default book (dense levels), on the tree book and on the
// naive RefBook oracle. Any difference in results, trades, order counts or best prices, or a
// failed validate(), aborts with a message. Built only with clang: cmake -DLOB_FUZZ=ON (see
// scripts/fuzz.sh). Prints the number of operations checked when the process exits.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "lob/order_book.hpp"
#include "reference_book.hpp"

namespace {

std::uint64_t g_ops = 0;
struct Report {
  ~Report() { std::fprintf(stderr, "fuzz_book: %llu operations checked\n", (unsigned long long)g_ops); }
} g_report;

[[noreturn]] void die(const std::string& what) {
  std::fprintf(stderr, "fuzz_book: %s (operation %llu)\n", what.c_str(), (unsigned long long)g_ops);
  std::abort();
}

struct Input {
  const std::uint8_t* p;
  std::size_t n;
  std::uint8_t byte() {
    if (n == 0) return 0;
    --n;
    return *p++;
  }
};

template <class Book>
void check_same(const Book& book, const RefBook& ref, const char* name) {
  std::string why;
  if (!book.validate(&why)) die(std::string(name) + " validate: " + why);
  if (book.order_count() != ref.size()) die(std::string(name) + " order count differs");
  if (book.best_bid() != ref.best(lob::Side::Buy) || book.best_ask() != ref.best(lob::Side::Sell))
    die(std::string(name) + " best prices differ");
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  using namespace lob;
  // Capacity covers every add a 4 KiB input can make, so the oracle (unbounded) never diverges
  // because of a full pool; the full-pool path has its own unit test.
  OrderBook dense(1u << 12);
  TreeOrderBook tree(1u << 12);
  RefBook ref;
  Input in{data, size};
  OrderId next = 1;
  Price base = 1'000'000;
  std::vector<Trade> ta, tb, tc;
  const auto into = [](std::vector<Trade>& v) { return [&v](const Trade& t) { v.push_back(t); }; };

  while (in.n > 0) {
    const std::uint8_t op = in.byte(), x = in.byte(), y = in.byte();
    ta.clear();
    tb.clear();
    tc.clear();
    const Side side = (op & 0x10) ? Side::Buy : Side::Sell;
    const Price price = base + static_cast<Price>(x % 64) - 32;
    const Qty qty = y == 0 ? 0 : 1 + y % 16;  // 0 exercises the reject paths
    const OrderId target = 1 + static_cast<OrderId>(x) % next;  // may be unknown: miss paths
    const Timestamp ts = g_ops;
    switch (op % 8) {
      case 0: case 1: case 2: case 3: {
        const OrderType type = (op % 8) == 3 ? OrderType::Market : OrderType::Limit;
        const OrderId id = next++;
        const auto a = dense.add(id, side, type, price, qty, ts, into(ta));
        const auto b = tree.add(id, side, type, price, qty, ts, into(tb));
        const auto c = ref.add(id, side, type, price, qty, ts, tc);
        if (a != c || b != c) die("add result differs");
        break;
      }
      case 4: {
        const bool c = ref.cancel(target);
        if (dense.cancel(target) != c || tree.cancel(target) != c) die("cancel result differs");
        break;
      }
      case 5: {
        const auto a = dense.modify(target, price, qty, ts, into(ta));
        const auto b = tree.modify(target, price, qty, ts, into(tb));
        const auto c = ref.modify(target, price, qty, ts, tc);
        if (a != c || b != c) die("modify result differs");
        break;
      }
      case 6: {
        const bool c = ref.reduce(target, y % 20);
        if (dense.reduce(target, y % 20) != c || tree.reduce(target, y % 20) != c)
          die("reduce result differs");
        break;
      }
      default:  // move the market far away: forces dense re-centring and overflow levels
        base += static_cast<Price>(static_cast<std::int8_t>(x)) * 1000;
        break;
    }
    if (ta != tc || tb != tc) die("trades differ");
    ++g_ops;
    check_same(dense, ref, "dense");
    check_same(tree, ref, "tree");
  }
  if (dense.checksum() != tree.checksum()) die("dense and tree checksums differ");
  return 0;
}
