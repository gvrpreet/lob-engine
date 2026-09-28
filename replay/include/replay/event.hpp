#pragma once
#include <cstdint>
#include <type_traits>

#include "lob/types.hpp"

namespace replay {

// Modify and Reduce exist from format v2 (magic LOBEVT02), Execute from v3 (LOBEVT03).
//   Modify: `id` changes to `price` / `qty` (otype Limit; side is ignored, the order keeps its own).
//   Reduce: `qty` shares come off order `id` (partial cancel, keeps queue position).
//   Execute: like Reduce for the book, but the shares were traded by an aggressor the feed does
//            not show (ITCH E/C). The trading replay uses it for queue-position fills.
enum class EventType : std::uint8_t { New = 0, Cancel = 1, Modify = 2, Reduce = 3, Execute = 4 };

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
