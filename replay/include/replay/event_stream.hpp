#pragma once
// Phase 5(b): streaming replay for logs bigger than memory.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "replay/event.hpp"
#include "replay/replayer.hpp"

namespace replay {

// Reads an event log (v1 or v2) through a fixed window of `window_events` records, so memory
// stays bounded whatever the file size. open() checks the header against the file size, like
// the other readers. Each chunk is validated before it is handed out, with the same error text
// (and absolute record index) as read_events / MappedEventLog, so a malformed record stops the
// replay before any event of its chunk is applied.
class EventLogStream {
 public:
  explicit EventLogStream(std::size_t window_events = std::size_t{1} << 20);

  bool open(const std::string& path, std::string* error = nullptr);
  // Next chunk (at most window_events records). An empty chunk means the end of the log.
  // Returns false on a read error or an invalid record.
  bool next(std::span<const Event>& chunk, std::string* error = nullptr);

  std::uint64_t count() const noexcept { return count_; }        // records in the file
  std::uint64_t consumed() const noexcept { return consumed_; }  // records handed out so far
  unsigned version() const noexcept { return version_; }

 private:
  struct FileCloser {
    void operator()(std::FILE* f) const noexcept;
  };
  std::unique_ptr<std::FILE, FileCloser> file_;
  std::vector<Event> window_;
  std::uint64_t count_ = 0, consumed_ = 0;
  unsigned version_ = 0;
};

// Replays the whole stream through the default book, with the same results and statistics as
// replay() on the same records. Per-event latency and book-shape measurement are not supported
// (cfg's measure flags are ignored); `seconds` includes reading the file. Returns false with a
// reason if the log is malformed; `out` then covers the events applied before the bad chunk.
bool replay_stream(EventLogStream& stream, const ReplayConfig& cfg, ReplayStats& out,
                   IStrategy* strategy = nullptr, std::string* error = nullptr);

}  // namespace replay
