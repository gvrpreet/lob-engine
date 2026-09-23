#include <cstdio>
#include <cstring>
#include <string>

#include "replay/event_log.hpp"
#include "replay/replayer.hpp"
#include "replay/strategies.hpp"
#include "replay/synthetic.hpp"
#include "testing.hpp"

TEST(generator_is_deterministic) {
  const auto a = replay::generate_events(10'000, 7);
  const auto b = replay::generate_events(10'000, 7);
  CHECK(a.size() == b.size());
  CHECK(std::memcmp(a.data(), b.data(), a.size() * sizeof(replay::Event)) == 0);
}

TEST(replay_is_deterministic) {
  const auto ev = replay::generate_events(200'000, 3);
  const auto s1 = replay::replay(ev);
  const auto s2 = replay::replay(ev);
  CHECK(s1.checksum == s2.checksum);
  CHECK(s1.trades == s2.trades && s1.volume == s2.volume);
  CHECK(s1.events == ev.size());
  CHECK(s1.trades > 0);
}

TEST(event_log_roundtrip) {
  const auto ev = replay::generate_events(5'000, 11);
  const std::string path = "/tmp/lob_test_events.bin";
  CHECK(replay::write_events(path, ev));
  std::vector<replay::Event> back;
  CHECK(replay::read_events(path, back));
  CHECK(back.size() == ev.size());
  CHECK(std::memcmp(ev.data(), back.data(), ev.size() * sizeof(replay::Event)) == 0);
  std::remove(path.c_str());
}

TEST(strategy_hook_sees_every_trade) {
  const auto ev = replay::generate_events(50'000, 5);
  replay::VwapTracker v;
  const auto st = replay::replay(ev, {}, &v);
  CHECK(st.trades > 0);
  CHECK(v.vwap() > 9'000 && v.vwap() < 11'000);
}
