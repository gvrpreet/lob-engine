#pragma once
#include <cstddef>
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

struct Order48 : Order {
  std::byte padding[16];
};

struct alignas(64) Order64 : Order {
  std::byte padding[32];
};

static_assert(sizeof(Order48) == 48);
static_assert(sizeof(Order64) == 64 && alignof(Order64) == 64);
static_assert(std::is_trivially_copyable_v<Order48> && std::is_trivially_copyable_v<Order64>);

}  // namespace lob
