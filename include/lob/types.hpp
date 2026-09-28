#pragma once
#include <cstdint>
#include <limits>

namespace lob {

using OrderId = std::uint64_t;
using Price = std::int64_t;      // integer ticks. Never floating point.
using Qty = std::uint32_t;
using Timestamp = std::uint64_t; // nanoseconds
using Handle = std::uint32_t;    // index into OrderPool (4 bytes instead of an 8-byte pointer)

inline constexpr Handle kInvalid = std::numeric_limits<Handle>::max();

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };
enum class OrderType : std::uint8_t { Limit = 0, Market = 1 };

struct Trade {
  OrderId taker;
  OrderId maker;
  Price price;  // always the resting (maker) order's price
  Qty qty;
  Timestamp ts;
  bool operator==(const Trade&) const = default;
};

enum class AddResult : std::uint8_t {
  Rejected,        // zero qty, duplicate id, or pool exhausted
  Rested,          // no fills, whole quantity now resting
  PartiallyRested, // some fills, remainder resting
  Filled,          // completely filled
  Discarded        // market order: unfilled remainder dropped (never rests)
};

enum class ModifyResult : std::uint8_t {
  NotFound,        // id is not resting
  Rejected,        // zero qty; the order is left unchanged
  Amended,         // same price, qty lowered or unchanged: queue position kept
  Rested,          // re-queued at the back of its (new) level, no fills
  PartiallyRested, // re-queued after some fills
  Filled           // the re-entered order traded away completely
};

}  // namespace lob
