// Phase 5(e): timestamp-paced replay changes timing only, never results.
#include <chrono>
#include <string>
#include <vector>

#include "replay/event_log.hpp"
#include "replay/paced.hpp"
#include "replay/replayer.hpp"
#include "testing.hpp"

TEST(paced_replay_matches_plain_replay) {
  for (const char* name : {"/golden_2000_seed7.bin", "/golden_v2_2000_seed7.bin"}) {
    std::vector<replay::Event> ev;
    CHECK(replay::read_events(std::string(LOB_TEST_DATA_DIR) + name, ev));
    const auto p = replay::replay_paced(ev, {}, 1e12);  // effectively no waiting
    const auto r = replay::replay(ev);
    CHECK(p.replay.checksum == r.checksum && p.replay.trades == r.trades &&
          p.replay.volume == r.volume && p.replay.rejected == r.rejected &&
          p.replay.cancel_misses == r.cancel_misses && p.replay.modify_misses == r.modify_misses &&
          p.replay.reduce_misses == r.reduce_misses && p.replay.final_orders == r.final_orders);
    CHECK(p.lateness.count() == ev.size());
  }
}

TEST(paced_replay_follows_timestamps) {
  // Five events 10 ms apart: a 40 ms log.
  std::vector<replay::Event> ev;
  for (int i = 0; i < 5; ++i) {
    replay::Event e{};
    e.ts = 1'000'000'000ULL + static_cast<std::uint64_t>(i) * 10'000'000ULL;
    e.id = static_cast<lob::OrderId>(i + 1);
    e.price = 100 - i;
    e.qty = 1;
    ev.push_back(e);
  }
  for (const double speed : {1.0, 2.0}) {
    const auto start = std::chrono::steady_clock::now();
    const auto p = replay::replay_paced(ev, {}, speed);
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    CHECK(p.log_span_s > 0.0399 && p.log_span_s < 0.0401);
    CHECK(wall >= 0.040 / speed);  // never early
    CHECK(wall < 1.0);             // and not absurdly late
    CHECK(p.replay.events == 5 && p.replay.final_orders == 5);
  }
  CHECK(replay::replay_paced({}, {}, 1.0).replay.events == 0);
}
