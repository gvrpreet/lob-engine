#pragma once
#include <span>
#include <string>
#include <vector>

#include "replay/event.hpp"

namespace replay {

// File format: 8-byte magic "LOBEVT01", u64 count, then `count` raw Event records.
bool write_events(const std::string& path, std::span<const Event> events);
bool read_events(const std::string& path, std::vector<Event>& out);

}  // namespace replay
