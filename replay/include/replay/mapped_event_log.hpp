#pragma once
#include <cstddef>
#include <span>
#include <string>

#include "replay/event.hpp"

namespace replay {

// Read-only memory mapping of an event log (phase 5a): events() views the records in place, with no
// copy and no allocation. The file is validated like read_events (header vs size, then every record),
// and it is mapped with MAP_POPULATE, since validation touches every page anyway.
// The span stays valid until the object is destroyed or moved from. POSIX only; elsewhere open() fails.
class MappedEventLog {
 public:
  MappedEventLog() = default;
  ~MappedEventLog();
  MappedEventLog(MappedEventLog&& other) noexcept;
  MappedEventLog& operator=(MappedEventLog&& other) noexcept;
  MappedEventLog(const MappedEventLog&) = delete;
  MappedEventLog& operator=(const MappedEventLog&) = delete;

  // Replaces any current mapping. On failure the object is left empty and `error` holds a reason.
  bool open(const std::string& path, std::string* error = nullptr);
  void close() noexcept;

  std::span<const Event> events() const noexcept { return events_; }
  bool is_open() const noexcept { return base_ != nullptr; }

 private:
  void* base_ = nullptr;
  std::size_t length_ = 0;
  std::span<const Event> events_;
};

}  // namespace replay
