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
// Format v2 golden (gen_events ... 2000 7 15): 1343 New, 339 Cancel, 231 Modify, 87 Reduce.
const std::string kGoldenV2 = std::string(LOB_TEST_DATA_DIR) + "/golden_v2_2000_seed7.bin";
constexpr std::uint64_t kGoldenV2Checksum = 0x91319d83ac94152cULL;

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
                              replay::replay_tree(ev), replay::replay_sorted(ev),
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

TEST(golden_v2_log_replays_to_known_results_on_every_variant) {
  std::vector<replay::Event> ev;
  CHECK(replay::read_events(kGoldenV2, ev));
  CHECK(ev.size() == 2000);

  const auto st = replay::replay(ev);
  CHECK(st.checksum == kGoldenV2Checksum);
  CHECK(st.trades == 489 && st.volume == 12'124);
  CHECK(st.rejected == 0 && st.cancel_misses == 98 && st.final_orders == 593);
  CHECK(st.modify_misses == 68 && st.reduce_misses == 21);

  for (const auto& variant : {replay::replay_naive(ev), replay::replay_order48(ev),
                              replay::replay_order64(ev), replay::replay_hotcold(ev),
                              replay::replay_tree(ev), replay::replay_sorted(ev),
                              replay::replay_hugepages(ev)}) {
    CHECK(variant.checksum == kGoldenV2Checksum);
    CHECK(variant.trades == st.trades && variant.volume == st.volume);
    CHECK(variant.modify_misses == st.modify_misses && variant.reduce_misses == st.reduce_misses);
  }

  replay::MappedEventLog mapped;
  CHECK(mapped.open(kGoldenV2));
  CHECK(replay::replay(mapped.events()).checksum == kGoldenV2Checksum);
}

TEST(generator_still_produces_golden_v2_log) {
  const auto ev = replay::generate_events(2000, 7, 15);
  const std::string path = temp_path("lob_regen_events_v2.bin");
  CHECK(replay::write_events(path, ev));
  CHECK(file_bytes(path) == file_bytes(kGoldenV2));
  std::remove(path.c_str());
}

TEST(v1_log_rejects_v2_event_types) {
  auto v1 = file_bytes(kGolden);
  const std::size_t rec = replay::kEventLogHeaderBytes + 5 * sizeof(replay::Event);
  v1[rec + offsetof(replay::Event, type)] = static_cast<char>(replay::EventType::Modify);
  CHECK(rejected(v1));  // Modify needs the v2 magic

  auto v2 = v1;
  v2[7] = '2';  // same bytes under the v2 magic are valid
  const std::string path = temp_path("lob_v2_events.bin");
  write_bytes(path, v2);
  std::vector<replay::Event> out;
  CHECK(replay::read_events(path, out) && out.size() == 2000);
  std::remove(path.c_str());

  auto bad_v2 = file_bytes(kGoldenV2);
  bad_v2[rec + offsetof(replay::Event, type)] = static_cast<char>(replay::EventType::Execute);
  CHECK(rejected(bad_v2));  // Execute needs the v3 magic

  auto v3 = bad_v2;
  v3[7] = '3';
  const std::string v3_path = temp_path("lob_v3_events.bin");
  write_bytes(v3_path, v3);
  std::vector<replay::Event> v3_events;
  CHECK(replay::read_events(v3_path, v3_events) && v3_events.size() == 2000);
  std::remove(v3_path.c_str());
  v3[rec + offsetof(replay::Event, type)] = 5;
  CHECK(rejected(v3));
}

TEST(writer_picks_the_lowest_version_that_fits) {
  const std::string path = temp_path("lob_version_events.bin");
  std::vector<replay::Event> ev(3);
  for (auto [type, magic] : {std::pair{replay::EventType::Cancel, '1'},
                             std::pair{replay::EventType::Reduce, '2'},
                             std::pair{replay::EventType::Execute, '3'}}) {
    ev[1].type = type;
    CHECK(replay::write_events(path, ev));
    const auto bytes = file_bytes(path);
    CHECK(bytes.size() > 8 && bytes[7] == magic);
    std::vector<replay::Event> back;
    CHECK(replay::read_events(path, back) && back.size() == 3 && back[1].type == type);
  }
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
  bad_magic[7] = '9';  // '2' is now the valid v2 magic
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
