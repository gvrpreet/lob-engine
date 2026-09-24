// usage: gen_events <out.bin> [count=5000000] [seed=42]
#include <cstdio>
#include <cstdlib>

#include "replay/event_log.hpp"
#include "replay/synthetic.hpp"

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "usage: %s <out.bin> [count] [seed]\n", argv[0]); return 2; }
  const std::size_t n = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 5'000'000;
  const std::uint64_t seed = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 42;
  const auto ev = replay::generate_events(n, seed);
  if (!replay::write_events(argv[1], ev)) { std::fprintf(stderr, "write failed\n"); return 1; }
  std::printf("wrote %zu events (%.1f MB) to %s\n", ev.size(),
              static_cast<double>(ev.size() * sizeof(replay::Event)) / 1e6, argv[1]);
}



// Verified working on Ubuntu 24.04, checked replay framework got book checksum
// Testing results 

// wrote 1000000 events (32.0 MB) to data/events_1M.bin
// events        : 1000000
// trades        : 528626 (volume 13489553)
// rejected      : 0   cancel misses: 120860   resting at end: 185922
// throughput    : 10.04 M events/s (0.100 s)
// vwap / last   : 9984.045 / 9996
// book checksum : ba7911b3a1dd1d8a
// latency p50    : 46 ns
// latency p90    : 149 ns
// latency p99    : 483 ns
// latency p99.9  : 1354 ns