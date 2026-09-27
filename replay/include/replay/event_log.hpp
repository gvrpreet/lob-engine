#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "replay/event.hpp"

namespace replay {

// Event log file format v1 (all integers little-endian; see docs/01-architecture.md):
//   offset 0   8 bytes   magic "LOBEVT01"
//   offset 8   u64       record count N
//   offset 16  N × 32    raw Event records (layout in event.hpp), nothing after them
// A valid file is exactly 16 + 32·N bytes, and every record has in-range enum bytes and
// reserved == 0. The records start at a 16-byte offset, so a page-aligned mapping of the
// file is correctly aligned for Event.
inline constexpr std::size_t kEventLogHeaderBytes = 16;

bool write_events(const std::string& path, std::span<const Event> events);

// Reads and validates a whole log. On failure returns false, leaves `out` empty, and (if given)
// sets `error` to a one-line reason.
bool read_events(const std::string& path, std::vector<Event>& out, std::string* error = nullptr);

// Index of the first record with an out-of-range enum byte or non-zero reserved byte;
// events.size() if all are valid.
std::size_t first_invalid_event(std::span<const Event> events) noexcept;

}  // namespace replay
