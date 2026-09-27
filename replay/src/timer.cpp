#include "replay/timer.hpp"

#include <algorithm>
#include <vector>

#include "replay/latency_histogram.hpp"

#if REPLAY_HAS_RDTSCP
#include <cpuid.h>
#endif

namespace replay {

namespace {

double calibrate_ns_per_tick() {
  using clock = std::chrono::steady_clock;
  // Median of a few short windows, so one preempted window cannot skew the scale.
  std::vector<double> samples;
  for (int i = 0; i < 5; ++i) {
    const auto t0 = clock::now();
    const auto c0 = TscTimer::read();
    while (clock::now() - t0 < std::chrono::milliseconds(10)) {
    }
    const auto c1 = TscTimer::read();
    const auto t1 = clock::now();
    samples.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count() /
                      static_cast<double>(c1 - c0));
  }
  std::sort(samples.begin(), samples.end());
  return samples[samples.size() / 2];
}

template <class Timer>
std::uint64_t overhead_ns(std::size_t samples) {
  const double scale = Timer::ns_per_unit();
  LatencyHistogram h;
  for (std::size_t i = 0; i < samples; ++i) {
    const auto start = Timer::read();
    const auto stop = Timer::read();
    h.record(static_cast<std::uint64_t>(static_cast<double>(stop - start) * scale));
  }
  return h.percentile(50);
}

}  // namespace

const char* timer_name(LatencyTimer timer) {
  return timer == LatencyTimer::Tsc ? "rdtscp" : "steady_clock";
}

bool tsc_available() { return REPLAY_HAS_RDTSCP != 0; }

bool tsc_invariant() {
#if REPLAY_HAS_RDTSCP
  unsigned eax, ebx, ecx, edx;
  if (!__get_cpuid(0x80000007u, &eax, &ebx, &ecx, &edx)) return false;
  return (edx & (1u << 8)) != 0;
#else
  return false;
#endif
}

double tsc_ns_per_tick() {
  static const double ns_per_tick = REPLAY_HAS_RDTSCP ? calibrate_ns_per_tick() : 1.0;
  return ns_per_tick;
}

std::uint64_t timer_overhead_ns(LatencyTimer timer, std::size_t samples) {
  return timer == LatencyTimer::Tsc ? overhead_ns<TscTimer>(samples)
                                    : overhead_ns<SteadyTimer>(samples);
}

}  // namespace replay
