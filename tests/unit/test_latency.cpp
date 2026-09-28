#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <vector>

#include "replay/latency_histogram.hpp"
#include "replay/replayer.hpp"
#include "replay/synthetic.hpp"
#include "replay/timer.hpp"
#include "testing.hpp"

using replay::LatencyHistogram;

TEST(histogram_small_values_are_exact) {
  for (std::uint64_t v = 0; v < 64; ++v) {
    const auto i = LatencyHistogram::bucket_index(v);
    CHECK(i == v);
    CHECK(LatencyHistogram::bucket_lowest(i) == v && LatencyHistogram::bucket_highest(i) == v);
  }
}

TEST(histogram_buckets_cover_values_within_one_32nd) {
  std::vector<std::uint64_t> values{64, 65, 100, 127, 128, 129, 1000, 4095, 4096, 123'456'789};
  for (unsigned bit = 6; bit < 64; ++bit) {
    const std::uint64_t p = std::uint64_t{1} << bit;
    values.insert(values.end(), {p - 1, p, p + 1});
  }
  values.push_back(std::numeric_limits<std::uint64_t>::max());
  for (const std::uint64_t v : values) {
    const auto i = LatencyHistogram::bucket_index(v);
    const auto lo = LatencyHistogram::bucket_lowest(i);
    const auto hi = LatencyHistogram::bucket_highest(i);
    CHECK(i < LatencyHistogram::kBucketCount);
    CHECK(lo <= v && v <= hi);
    CHECK(hi - lo <= lo / 32);  // relative bucket width <= 1/32
  }
  for (std::size_t i = 0; i + 1 < LatencyHistogram::kBucketCount; ++i) {
    CHECK(LatencyHistogram::bucket_highest(i) + 1 == LatencyHistogram::bucket_lowest(i + 1));
  }
  CHECK(LatencyHistogram::bucket_highest(LatencyHistogram::kBucketCount - 1) ==
        std::numeric_limits<std::uint64_t>::max());
}

TEST(histogram_empty_reports_zero) {
  const LatencyHistogram h;
  CHECK(h.count() == 0 && h.max() == 0 && h.percentile(50) == 0 && h.percentile(99.9) == 0);
}

TEST(histogram_percentiles_track_exact_order_statistics) {
  // Heavy-tailed deterministic sample: mostly tens of ns, occasional multi-microsecond outliers.
  std::uint64_t state = 0x9e3779b97f4a7c15ull;
  const auto next = [&] {
    state = state * 6364136223846793005ull + 1442695040888963407ull;
    return state >> 33;
  };
  std::vector<std::uint64_t> exact;
  LatencyHistogram h;
  for (int i = 0; i < 100'000; ++i) {
    const auto r = next();
    const std::uint64_t v = (r % 100 == 0) ? 1'000 + r % 50'000 : 20 + r % 200;
    exact.push_back(v);
    h.record(v);
  }
  std::sort(exact.begin(), exact.end());
  CHECK(h.count() == exact.size());
  CHECK(h.max() == exact.back());
  for (const double p : {0.0, 50.0, 90.0, 99.0, 99.9, 100.0}) {
    const auto k = static_cast<std::size_t>(p / 100.0 * static_cast<double>(exact.size() - 1));
    const std::uint64_t e = exact[k];
    const std::uint64_t got = h.percentile(p);
    CHECK(got >= e && got <= h.max());
    CHECK(e < 64 ? got == e : (got - e) * 32 <= e);
  }
}

TEST(tsc_timer_agrees_with_steady_clock) {
  if (!replay::tsc_available()) return;
  const double ns_per_tick = replay::tsc_ns_per_tick();
  CHECK(ns_per_tick > 0);

  using clock = std::chrono::steady_clock;
  const auto t0 = clock::now();
  const auto c0 = replay::TscTimer::read();
  while (clock::now() - t0 < std::chrono::milliseconds(20)) {
  }
  const auto c1 = replay::TscTimer::read();
  const double steady_ns = std::chrono::duration<double, std::nano>(clock::now() - t0).count();
  const double tsc_ns = static_cast<double>(c1 - c0) * ns_per_tick;
  CHECK(tsc_ns > 0.8 * steady_ns && tsc_ns < 1.25 * steady_ns);
}

TEST(timer_overhead_is_measurable) {
  for (const auto timer : {replay::LatencyTimer::SteadyClock, replay::LatencyTimer::Tsc}) {
    CHECK(replay::timer_overhead_ns(timer, 10'000) < 100'000);
  }
}

TEST(latency_measurement_does_not_change_replay) {
  const auto ev = replay::generate_events(50'000, 61);
  const auto plain = replay::replay(ev);
  CHECK(plain.latency.count() == 0);
  for (const auto timer : {replay::LatencyTimer::SteadyClock, replay::LatencyTimer::Tsc}) {
    replay::ReplayConfig cfg;
    cfg.measure_latency = true;
    cfg.latency_timer = timer;
    const auto timed = replay::replay(ev, cfg);
    const auto tree = replay::replay_tree(ev, cfg);
    CHECK(timed.checksum == plain.checksum && tree.checksum == plain.checksum);
    CHECK(timed.trades == plain.trades && timed.volume == plain.volume);
    CHECK(timed.latency.count() == ev.size() && tree.latency.count() == ev.size());
    CHECK(timed.latency_percentile(50) <= timed.latency_percentile(99));
    CHECK(timed.latency_percentile(99) <= timed.latency_percentile(99.9));
  }
}
