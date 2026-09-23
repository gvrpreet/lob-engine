#pragma once
#include <cstdint>
#include <type_traits>

#include "lob/types.hpp"

namespace replay {

enum class EventType : std::uint8_t { New = 0, Cancel = 1 };

// On-disk / in-memory exchange event. Fixed 32 bytes, trivially copyable, so a
// log file can be read (or later mmap'd) as a flat array with zero parsing.
struct Event {
  lob::Timestamp ts;
  lob::OrderId id;
  lob::Price price;
  lob::Qty qty;
  EventType type;
  lob::Side side;
  lob::OrderType otype;
  std::uint8_t reserved = 0;
};

static_assert(sizeof(Event) == 32);
static_assert(std::is_trivially_copyable_v<Event>);

}  // namespace replay
