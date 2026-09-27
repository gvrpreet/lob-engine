#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "replay/event_log.hpp"
#include "replay/mapped_event_log.hpp"
#include "replay/replayer.hpp"
#include "replay/synthetic.hpp"
#include "testing.hpp"

// Golden file: a committed 2,000-event log (gen_events ... 2000 7) with its expected replay results.
// It pins the engine independently of the generator: if only the generator test below fails, the
// generator changed; if the replay test fails, matching behaviour changed.
namespace {
const std::string kGolden = std::string(LOB_TEST_DATA_DIR) + "/golden_2000_seed7.bin";
constexpr std::uint64_t kGoldenChecksum = 0xc46a281df7e17679ULL;

std::vector<char> file_bytes(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string temp_path(const char* name) {
  return (std::filesystem::temp_directory_path() / name).string();
}

void write_bytes(const std::string& path, const std::vector<char>& bytes) {
  std::ofstream(path, std::ios::binary).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

// Writes `bytes` to a temp file and expects both readers to reject it with a reason.
bool rejected(const std::vector<char>& bytes) {
  const std::string path = temp_path("lob_bad_events.bin");
  write_bytes(path, bytes);
  std::vector<replay::Event> out(3);
  std::string read_error, map_error;
  const bool read_ok = replay::read_events(path, out, &read_error);
  replay::MappedEventLog mapped;
  const bool map_ok = mapped.open(path, &map_error);
  std::remove(path.c_str());
  return !read_ok && out.empty() && !read_error.empty() && !map_ok && !mapped.is_open() &&
         mapped.events().empty() && map_error == read_error;
}
}  // namespace

TEST(golden_log_replays_to_known_results_on_every_variant) {
  std::vector<replay::Event> ev;
  std::string error;
  CHECK(replay::read_events(kGolden, ev, &error));
  CHECK(ev.size() == 2000);

  const auto st = replay::replay(ev);
  CHECK(st.checksum == kGoldenChecksum);
  CHECK(st.trades == 576 && st.volume == 14'366);
  CHECK(st.rejected == 0 && st.cancel_misses == 109 && st.final_orders == 687);

  for (const auto& variant : {replay::replay_naive(ev), replay::replay_order48(ev),
                              replay::replay_order64(ev), replay::replay_hotcold(ev),
                              replay::replay_dense(ev), replay::replay_sorted(ev),
                              replay::replay_hugepages(ev)}) {
    CHECK(variant.checksum == kGoldenChecksum);
    CHECK(variant.trades == st.trades && variant.volume == st.volume);
  }
}

TEST(generator_still_produces_golden_log) {
  const auto ev = replay::generate_events(2000, 7);
  const std::string path = temp_path("lob_regen_events.bin");
  CHECK(replay::write_events(path, ev));
  CHECK(file_bytes(path) == file_bytes(kGolden));
  std::remove(path.c_str());
}

TEST(read_events_rejects_malformed_logs) {
  const std::vector<char> good = file_bytes(kGolden);
  CHECK(good.size() == replay::kEventLogHeaderBytes + 2000 * sizeof(replay::Event));

  auto truncated = good;
  truncated.resize(good.size() - 5);
  CHECK(rejected(truncated));

  auto trailing = good;
  trailing.push_back('\0');
  CHECK(rejected(trailing));

  auto bad_magic = good;
  bad_magic[7] = '2';
  CHECK(rejected(bad_magic));

  auto huge_count = good;  // header claims far more records than the file holds: no giant allocation
  const std::uint64_t n = std::uint64_t{1} << 60;
  std::memcpy(huge_count.data() + 8, &n, sizeof n);
  CHECK(rejected(huge_count));

  const std::size_t rec = replay::kEventLogHeaderBytes + 5 * sizeof(replay::Event);
  auto bad_type = good;
  bad_type[rec + offsetof(replay::Event, type)] = 7;
  CHECK(rejected(bad_type));

  auto bad_reserved = good;
  bad_reserved[rec + offsetof(replay::Event, reserved)] = 1;
  CHECK(rejected(bad_reserved));

  CHECK(rejected({'L', 'O', 'B'}));  // shorter than the header

  std::vector<replay::Event> out;
  std::string error;
  CHECK(!replay::read_events(temp_path("lob_missing_events.bin"), out, &error) && !error.empty());
}

TEST(read_events_accepts_empty_log) {
  const std::string path = temp_path("lob_empty_events.bin");
  CHECK(replay::write_events(path, {}));
  std::vector<replay::Event> out(3);
  CHECK(replay::read_events(path, out) && out.empty());
  replay::MappedEventLog mapped;
  CHECK(mapped.open(path) && mapped.is_open() && mapped.events().empty());
  CHECK(replay::replay(mapped.events()).events == 0);
  std::remove(path.c_str());
}

TEST(mapped_log_matches_read_events_and_replays_golden) {
  std::vector<replay::Event> copied;
  CHECK(replay::read_events(kGolden, copied));
  replay::MappedEventLog mapped;
  std::string error;
  CHECK(mapped.open(kGolden, &error));
  const auto view = mapped.events();
  CHECK(view.size() == copied.size());
  CHECK(std::memcmp(view.data(), copied.data(), copied.size() * sizeof(replay::Event)) == 0);
  CHECK(reinterpret_cast<std::uintptr_t>(view.data()) % alignof(replay::Event) == 0);
  CHECK(replay::replay(view).checksum == kGoldenChecksum);
}

TEST(mapped_log_move_transfers_ownership) {
  replay::MappedEventLog a;
  CHECK(a.open(kGolden));
  const replay::Event* first = a.events().data();
  replay::MappedEventLog b(std::move(a));
  CHECK(!a.is_open() && a.events().empty());  // NOLINT(bugprone-use-after-move): checking moved-from state
  CHECK(b.is_open() && b.events().data() == first && b.events().size() == 2000);
  replay::MappedEventLog c;
  CHECK(c.open(kGolden));
  c = std::move(b);  // releases c's own mapping, takes b's
  CHECK(c.events().data() == first && !b.is_open());
  CHECK(replay::replay(c.events()).checksum == kGoldenChecksum);
  c.close();
  CHECK(!c.is_open() && c.events().empty());
}
