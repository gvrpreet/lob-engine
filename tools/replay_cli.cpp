// usage: replay_cli <events.bin> [--latency]
#include <cstdio>
#include <cstring>

#include "replay/event_log.hpp"
#include "replay/replayer.hpp"
#include "replay/strategies.hpp"

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "usage: %s <events.bin> [--latency]\n", argv[0]); return 2; }
  std::vector<replay::Event> ev;
  if (!replay::read_events(argv[1], ev)) { std::fprintf(stderr, "cannot read %s\n", argv[1]); return 1; }

  replay::ReplayConfig cfg;
  cfg.measure_latency = argc > 2 && std::strcmp(argv[2], "--latency") == 0;
  replay::VwapTracker vwap;
  const auto st = replay::replay(ev, cfg, &vwap);

  std::printf("events        : %llu\n", (unsigned long long)st.events);
  std::printf("trades        : %llu (volume %llu)\n", (unsigned long long)st.trades,
              (unsigned long long)st.volume);
  std::printf("rejected      : %llu   cancel misses: %llu   resting at end: %llu\n",
              (unsigned long long)st.rejected, (unsigned long long)st.cancel_misses,
              (unsigned long long)st.final_orders);
  std::printf("throughput    : %.2f M events/s (%.3f s)\n", st.events_per_sec() / 1e6, st.seconds);
  std::printf("vwap / last   : %.3f / %lld\n", vwap.vwap(), (long long)vwap.last());
  std::printf("book checksum : %016llx\n", (unsigned long long)st.checksum);
  if (cfg.measure_latency) {
    for (double p : {50.0, 90.0, 99.0, 99.9})
      std::printf("latency p%-5g : %llu ns\n", p, (unsigned long long)st.latency_percentile(p));
  }
}
