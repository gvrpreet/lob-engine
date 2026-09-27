// Usage: bench_match [event-count] [controlled-active-level-count]. Generation and shape passes are excluded from timing.
// For trustworthy numbers follow docs/02-workflow.md ("Benchmark protocol").
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "replay/replayer.hpp"
#include "replay/synthetic.hpp"

int main(int argc, char** argv) {
  const std::size_t n = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 2'000'000;
  const std::size_t active_levels = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 0;
  const auto trade_percent = argc > 3 ? static_cast<std::uint32_t>(std::strtoul(argv[3], nullptr, 10)) : 0;
  if (trade_percent > 100 || (trade_percent != 0 && active_levels == 0)) {
    std::fprintf(stderr, "usage: bench_match [event-count] [active-level-count] [market-trade-percent]\n");
    return 2;
  }
  const auto ev = active_levels == 0 ? replay::generate_events(n)
                                     : replay::generate_depth_events(n, active_levels, 42,
                                                                     trade_percent);

  replay::ReplayConfig shape_config;
  shape_config.measure_book_shape = true;
  const auto shape = replay::replay(ev, shape_config);

  replay::ReplayConfig warm;
  (void)replay::replay(ev, warm);
  (void)replay::replay_naive(ev, warm);
  (void)replay::replay_order48(ev, warm);
  (void)replay::replay_order64(ev, warm);
  (void)replay::replay_hotcold(ev, warm);
  (void)replay::replay_dense(ev, warm);
  (void)replay::replay_sorted(ev, warm);
  (void)replay::replay_hugepages(ev, warm);

  std::array<std::vector<double>, 8> rates;
  std::array<std::uint64_t, 8> checksums{};
  const auto run_case = [&](std::size_t index) {
    switch (index) {
      case 0: return replay::replay(ev, warm);
      case 1: return replay::replay_naive(ev, warm);
      case 2: return replay::replay_order48(ev, warm);
      case 3: return replay::replay_order64(ev, warm);
      case 4: return replay::replay_hotcold(ev, warm);
      case 5: return replay::replay_dense(ev, warm);
      case 6: return replay::replay_sorted(ev, warm);
      default: return replay::replay_hugepages(ev, warm);
    }
  };
  for (int i = 0; i < 7; ++i) {
    for (std::size_t offset = 0; offset < rates.size(); ++offset) {
      const std::size_t index = (static_cast<std::size_t>(i) + offset) % rates.size();
      const auto stats = run_case(index);
      rates[index].push_back(stats.events_per_sec());
      checksums[index] = stats.checksum;
    }
  }
  for (auto& sample : rates) std::sort(sample.begin(), sample.end());
  for (std::uint64_t checksum : checksums) {
    if (checksum != checksums[0]) {
      std::fprintf(stderr, "checksum mismatch across order storage variants\n");
      return 1;
    }
  }
  constexpr std::array<const char*, 8> labels{"Order 32B", "new/delete", "Order 48B",
                                               "Order 64B", "hot/cold", "dense levels",
                                               "sorted levels", "32B huge"};
  std::printf("events=%zu requested_levels=%zu trade_percent=%u runs=7 checksum=%016llx trades=%llu final_levels=%llu peak_levels=%llu\n",
              n, active_levels, trade_percent,
              (unsigned long long)checksums[0], (unsigned long long)shape.trades,
              (unsigned long long)shape.final_levels,
              (unsigned long long)shape.peak_levels);
  for (std::size_t index = 0; index < rates.size(); ++index) {
    std::printf("%-12s median=%.2fM events/s (min=%.2fM max=%.2fM)\n", labels[index],
                rates[index][3] / 1e6, rates[index].front() / 1e6, rates[index].back() / 1e6);
  }

  if (replay::tsc_available()) {
    std::printf("tsc: %.4f ns/tick (%.3f GHz), invariant=%s\n", replay::tsc_ns_per_tick(),
                1.0 / replay::tsc_ns_per_tick(), replay::tsc_invariant() ? "yes" : "no");
  }
  for (const auto timer : {replay::LatencyTimer::SteadyClock, replay::LatencyTimer::Tsc}) {
    replay::ReplayConfig lat;
    lat.measure_latency = true;
    lat.latency_timer = timer;
    const auto st = replay::replay(ev, lat);
    if (st.checksum != checksums[0]) {
      std::fprintf(stderr, "checksum mismatch in latency run\n");
      return 1;
    }
    std::printf("latency ns %-12s (timer overhead p50=%llu, included): p50=%llu p90=%llu p99=%llu p99.9=%llu\n",
                replay::timer_name(timer), (unsigned long long)replay::timer_overhead_ns(timer),
                (unsigned long long)st.latency_percentile(50), (unsigned long long)st.latency_percentile(90),
                (unsigned long long)st.latency_percentile(99), (unsigned long long)st.latency_percentile(99.9));
  }
}
