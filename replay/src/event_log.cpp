#include "replay/event_log.hpp"

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

constexpr char kMagic[8] = {'L', 'O', 'B', 'E', 'V', 'T', '0', '1'};
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
  // An empty span may have data() == nullptr, which fwrite must not receive even for 0 records.
  return std::fwrite(kMagic, 1, 8, f.get()) == 8 && std::fwrite(&n, sizeof n, 1, f.get()) == 1 &&
         (events.empty() ||
          std::fwrite(events.data(), sizeof(Event), events.size(), f.get()) == events.size());
}

std::size_t first_invalid_event(std::span<const Event> events) noexcept {
  for (std::size_t i = 0; i < events.size(); ++i) {
    const Event& e = events[i];
    if (static_cast<std::uint8_t>(e.type) > 1 || static_cast<std::uint8_t>(e.side) > 1 ||
        static_cast<std::uint8_t>(e.otype) > 1 || e.reserved != 0)
      return i;
  }
  return events.size();
}

bool read_events(const std::string& path, std::vector<Event>& out, std::string* error) {
  out.clear();
  File f(std::fopen(path.c_str(), "rb"));
  if (!f) return fail(error, "cannot open " + path);

  char magic[8];
  std::uint64_t n = 0;
  if (std::fread(magic, 1, 8, f.get()) != 8 || std::fread(&n, sizeof n, 1, f.get()) != 1)
    return fail(error, "file shorter than the 16-byte header");
  if (std::memcmp(magic, kMagic, 8) != 0) return fail(error, "bad magic (expected LOBEVT01)");

  // Check the header count against the real size before allocating anything.
  if (std::fseek(f.get(), 0, SEEK_END) != 0) return fail(error, "cannot seek");
  const long end = std::ftell(f.get());
  if (end < 0) return fail(error, "cannot determine file size");
  const auto payload = static_cast<std::uint64_t>(end) - kEventLogHeaderBytes;
  if (payload % sizeof(Event) != 0 || n != payload / sizeof(Event))
    return fail(error, "header says " + std::to_string(n) + " records but the file holds " +
                           std::to_string(payload) + " payload bytes (" +
                           std::to_string(payload / sizeof(Event)) + " whole records)");
  if (std::fseek(f.get(), static_cast<long>(kEventLogHeaderBytes), SEEK_SET) != 0)
    return fail(error, "cannot seek");

  out.resize(static_cast<std::size_t>(n));
  if (!out.empty() && std::fread(out.data(), sizeof(Event), out.size(), f.get()) != out.size()) {
    out.clear();
    return fail(error, "short read");
  }
  if (const std::size_t bad = first_invalid_event(out); bad != out.size()) {
    out.clear();
    return fail(error, "record " + std::to_string(bad) + " has an invalid type/side/order-type or reserved byte");
  }
  return true;
}

}  // namespace replay
