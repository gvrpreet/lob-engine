#include "lob/order_book.hpp"

namespace lob {

std::uint64_t OrderBook::checksum() const noexcept {
  std::uint64_t h = 1469598103934665603ULL;  // FNV-1a
  const auto mix = [&h](std::uint64_t v) {
    h ^= v;
    h *= 1099511628211ULL;
  };
  const auto walk = [&](const auto& book, std::uint64_t tag) {
    for (const auto& [price, level] : book) {
      mix(tag);
      mix(static_cast<std::uint64_t>(price));
      for (Handle c = level.head; c != kInvalid; c = pool_[c].next) {
        mix(pool_[c].id);
        mix(pool_[c].qty);
      }
    }
  };
  walk(bids_, 1);
  walk(asks_, 2);
  return h;
}

}  // namespace lob
