#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "replay/event.hpp"

namespace replay {

// Event log file format (all integers little-endian; see docs/01-architecture.md):
//   offset 0   8 bytes   magic "LOBEVT01" (v1: New/Cancel) or "LOBEVT02" (v2: adds Modify/Reduce)
//   offset 8   u64       record count N
//   offset 16  N × 32    raw Event records (layout in event.hpp), nothing after them
// A valid file is exactly 16 + 32·N bytes, and every record has enum bytes in range for its
// version and reserved == 0. The records start at a 16-byte offset, so a page-aligned mapping of
// the file is correctly aligned for Event.
inline constexpr std::size_t kEventLogHeaderBytes = 16;

// Writes v1 when every event is New/Cancel (so v1 logs stay byte-identical), v2 otherwise.
bool write_events(const std::string& path, std::span<const Event> events);

// Reads and validates a whole log. On failure returns false, leaves `out` empty, and (if given)
// sets `error` to a one-line reason.
bool read_events(const std::string& path, std::vector<Event>& out, std::string* error = nullptr);

// Index of the first record with an enum byte out of range for format `version` (1 or 2) or a
// non-zero reserved byte; events.size() if all are valid.
std::size_t first_invalid_event(std::span<const Event> events, unsigned version = 2) noexcept;

// Shared by both readers: checks the 16-byte header against the whole file's size and returns the
// record count and format version. `header` must point at the file's first kEventLogHeaderBytes bytes.
bool check_event_log_header(const void* header, std::uint64_t file_bytes, std::uint64_t& count,
                            unsigned& version, std::string* error = nullptr);

}  // namespace replay
