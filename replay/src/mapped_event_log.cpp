#include "replay/mapped_event_log.hpp"

#include <cstdint>
#include <utility>

#include "replay/event_log.hpp"

#if defined(__unix__)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace replay {

MappedEventLog::~MappedEventLog() { close(); }

MappedEventLog::MappedEventLog(MappedEventLog&& other) noexcept
    : base_(std::exchange(other.base_, nullptr)),
      length_(std::exchange(other.length_, 0)),
      events_(std::exchange(other.events_, {})) {}

MappedEventLog& MappedEventLog::operator=(MappedEventLog&& other) noexcept {
  if (this != &other) {
    close();
    base_ = std::exchange(other.base_, nullptr);
    length_ = std::exchange(other.length_, 0);
    events_ = std::exchange(other.events_, {});
  }
  return *this;
}

void MappedEventLog::close() noexcept {
#if defined(__unix__)
  if (base_ != nullptr) (void)munmap(base_, length_);
#endif
  base_ = nullptr;
  length_ = 0;
  events_ = {};
}

bool MappedEventLog::open(const std::string& path, std::string* error) {
  close();
#if defined(__unix__)
  const auto fail = [error](std::string reason) {
    if (error) *error = std::move(reason);
    return false;
  };
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return fail("cannot open " + path);
  struct stat st {};
  if (::fstat(fd, &st) != 0) {
    ::close(fd);
    return fail("cannot stat " + path);
  }
  const auto file_bytes = static_cast<std::uint64_t>(st.st_size);
  if (file_bytes < kEventLogHeaderBytes) {  // also avoids a zero-length mmap, which is an error
    ::close(fd);
    std::uint64_t unused = 0;
    unsigned unused_version = 0;
    const char none[kEventLogHeaderBytes] = {};
    return check_event_log_header(none, file_bytes, unused, unused_version, error);
  }

  int flags = MAP_PRIVATE;
#if defined(MAP_POPULATE)
  flags |= MAP_POPULATE;
#endif
  const auto length = static_cast<std::size_t>(file_bytes);
  void* base = ::mmap(nullptr, length, PROT_READ, flags, fd, 0);
  ::close(fd);  // the mapping keeps the file referenced
  if (base == MAP_FAILED) return fail("mmap failed for " + path);

  std::uint64_t n = 0;
  unsigned version = 0;
  if (!check_event_log_header(base, file_bytes, n, version, error)) {
    (void)munmap(base, length);
    return false;
  }
  // Records start at offset 16 of a page-aligned mapping, so they are aligned for Event.
  const auto* first = reinterpret_cast<const Event*>(static_cast<const char*>(base) + kEventLogHeaderBytes);
  const std::span<const Event> events(first, static_cast<std::size_t>(n));
  if (const std::size_t bad = first_invalid_event(events, version); bad != events.size()) {
    (void)munmap(base, length);
    return fail("record " + std::to_string(bad) + " has an invalid type/side/order-type or reserved byte");
  }
  base_ = base;
  length_ = length;
  events_ = events;
  return true;
#else
  if (error) *error = "memory-mapped logs need a POSIX system; use read_events";
  (void)path;
  return false;
#endif
}

}  // namespace replay
