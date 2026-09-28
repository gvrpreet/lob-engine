#include "replay/event_log.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <utility>

namespace replay {
namespace {
static_assert(std::endian::native == std::endian::little,
              "event logs are little-endian raw records; a big-endian host needs byte swapping");

constexpr char kMagicV1[8] = {'L', 'O', 'B', 'E', 'V', 'T', '0', '1'};
constexpr char kMagicV2[8] = {'L', 'O', 'B', 'E', 'V', 'T', '0', '2'};
constexpr char kMagicV3[8] = {'L', 'O', 'B', 'E', 'V', 'T', '0', '3'};

// Highest event type each format version allows: v1 New/Cancel, v2 + Modify/Reduce, v3 + Execute.
constexpr std::uint8_t max_type_for(unsigned version) noexcept {
  return version >= 3 ? 4 : version == 2 ? 3 : 1;
}
struct FileCloser { void operator()(std::FILE* f) const { if (f) std::fclose(f); } };
using File = std::unique_ptr<std::FILE, FileCloser>;

bool fail(std::string* error, std::string reason) {
  if (error) *error = std::move(reason);
  return false;
}
}  // namespace

bool write_events(const std::string& path, std::span<const Event> events) {
  File f(std::fopen(path.c_str(), "wb"));
  if (!f) return false;
  const std::uint64_t n = events.size();
  // The lowest version that can hold every event, so older logs stay byte-identical.
  std::uint8_t max_type = 0;
  for (const Event& e : events) max_type = std::max(max_type, static_cast<std::uint8_t>(e.type));
  const char* magic = max_type > max_type_for(2) ? kMagicV3
                      : max_type > max_type_for(1) ? kMagicV2
                                                   : kMagicV1;
  // An empty span may have data() == nullptr, which fwrite must not receive even for 0 records.
  return std::fwrite(magic, 1, 8, f.get()) == 8 &&
         std::fwrite(&n, sizeof n, 1, f.get()) == 1 &&
         (events.empty() ||
          std::fwrite(events.data(), sizeof(Event), events.size(), f.get()) == events.size());
}

std::size_t first_invalid_event(std::span<const Event> events, unsigned version) noexcept {
  const std::uint8_t max_type = max_type_for(version);
  for (std::size_t i = 0; i < events.size(); ++i) {
    const Event& e = events[i];
    if (static_cast<std::uint8_t>(e.type) > max_type || static_cast<std::uint8_t>(e.side) > 1 ||
        static_cast<std::uint8_t>(e.otype) > 1 || e.reserved != 0)
      return i;
  }
  return events.size();
}

bool check_event_log_header(const void* header, std::uint64_t file_bytes, std::uint64_t& count,
                            unsigned& version, std::string* error) {
  if (file_bytes < kEventLogHeaderBytes) return fail(error, "file shorter than the 16-byte header");
  if (std::memcmp(header, kMagicV1, 8) == 0) version = 1;
  else if (std::memcmp(header, kMagicV2, 8) == 0) version = 2;
  else if (std::memcmp(header, kMagicV3, 8) == 0) version = 3;
  else return fail(error, "bad magic (expected LOBEVT01, LOBEVT02 or LOBEVT03)");
  std::uint64_t n = 0;
  std::memcpy(&n, static_cast<const char*>(header) + 8, sizeof n);
  const std::uint64_t payload = file_bytes - kEventLogHeaderBytes;
  if (payload % sizeof(Event) != 0 || n != payload / sizeof(Event))
    return fail(error, "header says " + std::to_string(n) + " records but the file holds " +
                           std::to_string(payload) + " payload bytes (" +
                           std::to_string(payload / sizeof(Event)) + " whole records)");
  count = n;
  return true;
}

bool read_events(const std::string& path, std::vector<Event>& out, std::string* error) {
  out.clear();
  File f(std::fopen(path.c_str(), "rb"));
  if (!f) return fail(error, "cannot open " + path);

  // Check the header count against the real size before allocating anything.
  if (std::fseek(f.get(), 0, SEEK_END) != 0) return fail(error, "cannot seek");
  const long end = std::ftell(f.get());
  if (end < 0) return fail(error, "cannot determine file size");
  if (std::fseek(f.get(), 0, SEEK_SET) != 0) return fail(error, "cannot seek");
  char header[kEventLogHeaderBytes] = {};
  if (std::fread(header, 1, sizeof header, f.get()) != sizeof header &&
      static_cast<std::uint64_t>(end) >= kEventLogHeaderBytes)
    return fail(error, "short read");  // a genuinely short file fails the size check below instead
  std::uint64_t n = 0;
  unsigned version = 0;
  if (!check_event_log_header(header, static_cast<std::uint64_t>(end), n, version, error)) return false;

  out.resize(static_cast<std::size_t>(n));
  if (!out.empty() && std::fread(out.data(), sizeof(Event), out.size(), f.get()) != out.size()) {
    out.clear();
    return fail(error, "short read");
  }
  if (const std::size_t bad = first_invalid_event(out, version); bad != out.size()) {
    out.clear();
    return fail(error, "record " + std::to_string(bad) + " has an invalid type/side/order-type or reserved byte");
  }
  return true;
}

}  // namespace replay
