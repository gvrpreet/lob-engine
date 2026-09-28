#pragma once
// Phase 5(d): Nasdaq TotalView-ITCH 5.0 subset -> Event log converter.
//
// Input is the "BinaryFILE" form Nasdaq publishes: each message is preceded by a 2-byte
// big-endian length. All ITCH integers are big-endian. Only one stock (by symbol) is kept.
//
// Mapping (the book reconstruction is exact for displayed orders):
//   R stock directory  -> learns the symbol's stock locate
//   A / F add order     -> New limit; engine id = the order's ITCH reference number
//   E / C executed      -> Reduce by the executed shares (the aggressor never appears as an Add)
//   X cancel            -> Reduce by the cancelled shares
//   D delete            -> Cancel
//   U replace           -> ITCH replace always loses priority. Emitted as Modify when the engine
//                          would also re-queue (new price or more shares), otherwise as
//                          Cancel + New. Either way the new reference maps to the same engine id.
// Prices stay in ITCH units (1/10000 dollar) as integer ticks; timestamps are ns since midnight.
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "replay/event.hpp"

namespace replay {

struct ItchOptions {
  std::string symbol;            // e.g. "AAPL" (padded with spaces internally)
  std::uint64_t max_events = 0;  // stop after this many output events (0 = whole file)
};

struct ItchStats {
  std::uint64_t messages = 0;       // all messages read
  std::uint64_t symbol_messages = 0;  // messages for the chosen stock
  std::uint64_t adds = 0, executions = 0, cancels = 0, deletes = 0, replaces = 0;
  std::uint64_t unknown_refs = 0;   // E/C/X/D/U for a reference never added (should be 0)
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
