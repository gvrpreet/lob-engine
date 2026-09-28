// Phase 5(b): streaming replay through a bounded window matches full-load replay exactly.
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "replay/event_log.hpp"
#include "replay/event_stream.hpp"
#include "replay/replayer.hpp"
#include "replay/strategies.hpp"
#include "testing.hpp"

namespace {

const std::string kDir = LOB_TEST_DATA_DIR;

std::vector<char> file_bytes(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string write_temp(const char* name, const std::vector<char>& bytes) {
  const std::string path = (std::filesystem::temp_directory_path() / name).string();
  std::ofstream(path, std::ios::binary).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  return path;
}

bool same_stats(const replay::ReplayStats& a, const replay::ReplayStats& b) {
  return a.checksum == b.checksum && a.events == b.events && a.trades == b.trades &&
         a.volume == b.volume && a.rejected == b.rejected && a.cancel_misses == b.cancel_misses &&
         a.modify_misses == b.modify_misses && a.reduce_misses == b.reduce_misses &&
         a.final_orders == b.final_orders;
}

}  // namespace

TEST(stream_matches_full_replay_at_every_window) {
  for (const char* name : {"/golden_2000_seed7.bin", "/golden_v2_2000_seed7.bin"}) {
    std::vector<replay::Event> ev;
    CHECK(replay::read_events(kDir + name, ev));
    const auto full = replay::replay(ev);
    for (const std::size_t window : {std::size_t{1}, std::size_t{7}, std::size_t{2000},
                                     std::size_t{1} << 20}) {
      replay::EventLogStream in(window);
      replay::ReplayStats st;
      std::string error;
      CHECK(in.open(kDir + name, &error));
      CHECK(replay::replay_stream(in, {}, st, nullptr, &error));
      CHECK(same_stats(st, full));
      CHECK(in.consumed() == in.count() && in.count() == 2000);
    }
  }
}

TEST(stream_with_strategy_sees_every_trade) {
  std::vector<replay::Event> ev;
  CHECK(replay::read_events(kDir + "/golden_2000_seed7.bin", ev));
  replay::VwapTracker a, b;
  replay::replay(ev, {}, &a);
  replay::EventLogStream in(13);
  replay::ReplayStats st;
  CHECK(in.open(kDir + "/golden_2000_seed7.bin"));
  CHECK(replay::replay_stream(in, {}, st, &b));
  CHECK(a.vwap() == b.vwap() && a.last() == b.last());
}

TEST(stream_rejects_malformed_logs_like_the_other_readers) {
  const std::vector<char> good = file_bytes(kDir + "/golden_2000_seed7.bin");

  // A bad record in the middle: chunks before it replay, then the stream stops with the same
  // message (and absolute index) as read_events.
  auto bad = good;
  const std::size_t rec = replay::kEventLogHeaderBytes + 1234 * sizeof(replay::Event);
  bad[rec + offsetof(replay::Event, type)] = 9;
  const std::string path = write_temp("lob_stream_bad.bin", bad);
  std::vector<replay::Event> out;
  std::string read_error, stream_error;
  CHECK(!replay::read_events(path, out, &read_error));
  replay::EventLogStream in(100);
  replay::ReplayStats st;
  CHECK(in.open(path));
  CHECK(!replay::replay_stream(in, {}, st, nullptr, &stream_error));
  CHECK(stream_error == read_error && stream_error.find("record 1234") != std::string::npos);
  CHECK(st.events == 1200);  // 12 whole chunks of 100 applied; the chunk holding 1234 was not
  std::remove(path.c_str());

  auto truncated = good;
  truncated.resize(good.size() - 5);
  const std::string tpath = write_temp("lob_stream_trunc.bin", truncated);
  replay::EventLogStream t;
  std::string terr;
  CHECK(!t.open(tpath, &terr) && !terr.empty());
  std::span<const replay::Event> chunk;
  CHECK(!t.next(chunk, &terr) && chunk.empty());  // not open
  std::remove(tpath.c_str());

  const std::string empty = (std::filesystem::temp_directory_path() / "lob_stream_empty.bin").string();
  CHECK(replay::write_events(empty, {}));
  replay::EventLogStream e;
  CHECK(e.open(empty) && e.count() == 0);
  CHECK(replay::replay_stream(e, {}, st) && st.events == 0);
  std::remove(empty.c_str());
}
