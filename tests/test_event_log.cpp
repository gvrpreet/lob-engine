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

// Writes `bytes` to a temp file and expects read_events to reject it with a reason.
bool rejected(const std::vector<char>& bytes) {
  const std::string path = temp_path("lob_bad_events.bin");
  write_bytes(path, bytes);
  std::vector<replay::Event> out(3);
  std::string error;
  const bool ok = replay::read_events(path, out, &error);
  std::remove(path.c_str());
  return !ok && out.empty() && !error.empty();
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
  std::remove(path.c_str());
}
