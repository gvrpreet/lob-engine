// Throughput + latency microbench over synthetic events. Generation is excluded from timing.
// For trustworthy numbers follow docs/02-workflow.md ("Benchmark protocol").
#include <algorithm>
#include <cstdio>
#include <vector>

#include "replay/replayer.hpp"
#include "replay/synthetic.hpp"

int main(int argc, char** argv) {
  const std::size_t n = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 2'000'000;
  const auto ev = replay::generate_events(n);

  replay::ReplayConfig warm;
  (void)replay::replay(ev, warm);  // warm caches, page-fault the pool

  std::vector<double> rates;
  std::uint64_t checksum = 0;
  for (int i = 0; i < 7; ++i) {
    const auto st = replay::replay(ev, warm);
    rates.push_back(st.events_per_sec());
    checksum = st.checksum;
  }
  std::sort(rates.begin(), rates.end());
  std::printf("events=%zu runs=7  min=%.2fM  median=%.2fM  max=%.2fM events/s  checksum=%016llx\n", n,
              rates.front() / 1e6, rates[3] / 1e6, rates.back() / 1e6,
              (unsigned long long)checksum);

  replay::ReplayConfig lat;
  lat.measure_latency = true;
  const auto st = replay::replay(ev, lat);
  std::printf("latency ns (incl. ~timer overhead): p50=%llu p90=%llu p99=%llu p99.9=%llu\n",
              (unsigned long long)st.latency_percentile(50), (unsigned long long)st.latency_percentile(90),
              (unsigned long long)st.latency_percentile(99), (unsigned long long)st.latency_percentile(99.9));
}
