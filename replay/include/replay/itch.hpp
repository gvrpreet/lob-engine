#pragma once
// Phase 5(d): Nasdaq TotalView-ITCH 5.0 subset -> Event log converter.
//
// Input is the "BinaryFILE" form Nasdaq publishes: each message is preceded by a 2-byte
// big-endian length. All ITCH integers are big-endian. Only one stock (by symbol) is kept.
//
// Mapping (the book reconstruction is exact for displayed orders):
//   R stock directory  -> learns the symbol's stock locate
//   A / F add order     -> New limit; engine id = the order's ITCH reference number
//   E / C executed      -> Execute of the executed shares (the aggressor never appears as an
//                          Add; Execute acts as Reduce on the book, and drives queue-position
//                          fills in the trading replay). Output is therefore a v3 log.
//   X cancel            -> Reduce by the cancelled shares
//   D delete            -> Cancel
//   U replace           -> ITCH replace always loses priority. Emitted as Modify when the engine
//                          would also re-queue (new price or more shares), otherwise as
//                          Cancel + New. Either way the new reference maps to the same engine id.
// Prices are ITCH units (1/10000 dollar) divided by `tick` (e.g. 100 = one-cent ticks, which
// stocks priced >= $1 are quoted in); a price that is not a multiple of `tick` is an error, so
// the conversion stays lossless. Timestamps are ns since midnight.
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "replay/event.hpp"

namespace replay {

struct ItchOptions {
  std::string symbol;            // e.g. "AAPL" (padded with spaces internally)
  std::uint64_t max_events = 0;  // stop after this many output events (0 = whole file)
  std::uint32_t tick = 1;        // ITCH price units per output tick (1 = keep 1/10000 $)
  // Off-grid prices fail the conversion by default. With this set, an order whose price is off
  // the grid is dropped (with all its later messages) and counted in off_grid_orders instead.
  // Real case: sub-dollar orders may be sub-penny (Reg NMS 612 only covers prices >= $1), e.g.
  // a $0.0001 bid in AAPL. Such orders are far from the touch, but dropping them is lossy.
  bool drop_off_grid = false;
};

struct ItchStats {
  std::uint64_t messages = 0;       // all messages read
  std::uint64_t symbol_messages = 0;  // messages for the chosen stock
  std::uint64_t adds = 0, executions = 0, cancels = 0, deletes = 0, replaces = 0;
  std::uint64_t unknown_refs = 0;   // E/C/X/D/U for a reference never added (should be 0)
  std::uint64_t off_grid_orders = 0;  // drop_off_grid: orders dropped for an off-grid price
  std::uint16_t locate = 0;         // the symbol's stock locate (0 = not found)
};

// Converts `path` for one symbol. Returns false with a reason on I/O or format errors, or when
// the symbol never appears in the stock directory.
bool convert_itch(const std::string& path, const ItchOptions& options, std::vector<Event>& out,
                  ItchStats& stats, std::string* error = nullptr);

// Counts Add (A/F) messages per symbol over the whole file, most active first.
bool itch_add_counts(const std::string& path,
                     std::vector<std::pair<std::string, std::uint64_t>>& out,
                     std::string* error = nullptr);

}  // namespace replay
