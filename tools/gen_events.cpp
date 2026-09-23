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
