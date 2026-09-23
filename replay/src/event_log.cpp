#include "replay/event_log.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>

namespace replay {
namespace {
constexpr char kMagic[8] = {'L', 'O', 'B', 'E', 'V', 'T', '0', '1'};
struct FileCloser { void operator()(std::FILE* f) const { if (f) std::fclose(f); } };
using File = std::unique_ptr<std::FILE, FileCloser>;
}  // namespace

bool write_events(const std::string& path, std::span<const Event> events) {
  File f(std::fopen(path.c_str(), "wb"));
  if (!f) return false;
  const std::uint64_t n = events.size();
  return std::fwrite(kMagic, 1, 8, f.get()) == 8 && std::fwrite(&n, sizeof n, 1, f.get()) == 1 &&
         std::fwrite(events.data(), sizeof(Event), events.size(), f.get()) == events.size();
}

bool read_events(const std::string& path, std::vector<Event>& out) {
  File f(std::fopen(path.c_str(), "rb"));
  if (!f) return false;
  char magic[8];
  std::uint64_t n = 0;
  if (std::fread(magic, 1, 8, f.get()) != 8 || std::memcmp(magic, kMagic, 8) != 0) return false;
  if (std::fread(&n, sizeof n, 1, f.get()) != 1) return false;
  out.resize(n);
  return std::fread(out.data(), sizeof(Event), n, f.get()) == n;
}

}  // namespace replay
