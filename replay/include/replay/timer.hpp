#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>

#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#define REPLAY_HAS_RDTSCP 1
#else
#define REPLAY_HAS_RDTSCP 0
#endif

namespace replay {

enum class LatencyTimer : std::uint8_t { SteadyClock, Tsc };

const char* timer_name(LatencyTimer timer);

// rdtscp support is compile-time (x86 only). Invariance (constant rate across P-states and
// C-states) comes from CPUID and is informational: VMs may hide or emulate it.
bool tsc_available();
bool tsc_invariant();
double tsc_ns_per_tick();  // calibrated once per process against steady_clock

// Median ns of an empty start/stop interval: the floor every per-event sample includes.
std::uint64_t timer_overhead_ns(LatencyTimer timer, std::size_t samples = 100'000);

struct SteadyTimer {
  static std::uint64_t read() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
  }
  static double ns_per_unit() { return 1.0; }
};

// rdtscp waits for earlier instructions to finish; the trailing lfence keeps later ones from
// starting before the counter is read. Falls back to steady_clock where rdtscp is unavailable.
struct TscTimer {
  static std::uint64_t read() {
#if REPLAY_HAS_RDTSCP
    unsigned aux;
    const std::uint64_t t = __rdtscp(&aux);
    _mm_lfence();
    return t;
#else
    return SteadyTimer::read();
#endif
  }
  static double ns_per_unit() { return REPLAY_HAS_RDTSCP ? tsc_ns_per_tick() : 1.0; }
};

}  // namespace replay
