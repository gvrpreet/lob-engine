#pragma once
#include <type_traits>

#include "lob/types.hpp"

namespace lob {

// A resting order. Exactly 32 bytes => two per 64-byte cache line.
// prev/next are pool handles forming an intrusive FIFO list per price level.
// While a slot is on the pool free list, `next` links free slots.
struct Order {
  OrderId id;
  Price price;
  Qty qty;
  Handle prev;
  Handle next;
  Side side;
};

static_assert(sizeof(Order) == 32, "Order layout drifted: re-check cache-line packing");
static_assert(std::is_trivially_copyable_v<Order>);

}  // namespace lob
