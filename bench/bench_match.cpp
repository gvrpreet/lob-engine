// Throughput + latency microbench over synthetic events. Generation is excluded from timing.
// For trustworthy numbers follow docs/02-workflow.md ("Benchmark protocol").
#include <algorithm>
#include <array>
#include <cstdio>
#include <vector>

#include "replay/replayer.hpp"
#include "replay/synthetic.hpp"

int main(int argc, char** argv) {
  const std::size_t n = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 2'000'000;
  const auto ev = replay::generate_events(n);

  replay::ReplayConfig warm;
  (void)replay::replay(ev, warm);
  (void)replay::replay_naive(ev, warm);
  (void)replay::replay_order48(ev, warm);
  (void)replay::replay_order64(ev, warm);
  (void)replay::replay_hotcold(ev, warm);
  (void)replay::replay_dense(ev, warm);

  std::array<std::vector<double>, 6> rates;
  std::array<std::uint64_t, 6> checksums{};
  const auto run_case = [&](std::size_t index) {
    switch (index) {
      case 0: return replay::replay(ev, warm);
      case 1: return replay::replay_naive(ev, warm);
      case 2: return replay::replay_order48(ev, warm);
      case 3: return replay::replay_order64(ev, warm);
      case 4: return replay::replay_hotcold(ev, warm);
      default: return replay::replay_dense(ev, warm);
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
  constexpr std::array<const char*, 6> labels{"Order 32B", "new/delete", "Order 48B",
                                               "Order 64B", "hot/cold", "dense levels"};
  std::printf("events=%zu runs=7 checksum=%016llx\n", n, (unsigned long long)checksums[0]);
  for (std::size_t index = 0; index < rates.size(); ++index) {
    std::printf("%-12s median=%.2fM events/s (min=%.2fM max=%.2fM)\n", labels[index],
                rates[index][3] / 1e6, rates[index].front() / 1e6, rates[index].back() / 1e6);
  }

  replay::ReplayConfig lat;
  lat.measure_latency = true;
  const auto st = replay::replay(ev, lat);
  std::printf("latency ns (incl. ~timer overhead): p50=%llu p90=%llu p99=%llu p99.9=%llu\n",
              (unsigned long long)st.latency_percentile(50), (unsigned long long)st.latency_percentile(90),
              (unsigned long long)st.latency_percentile(99), (unsigned long long)st.latency_percentile(99.9));
}
