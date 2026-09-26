// usage: replay_cli <events.bin> [--latency] [--naive] [--no-strategy] [--order-size 48|64] [--hot-cold]
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "replay/event_log.hpp"
#include "replay/replayer.hpp"
#include "replay/strategies.hpp"

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr,
                 "usage: %s <events.bin> [--latency] [--naive] [--no-strategy] [--order-size 48|64] [--hot-cold]\n",
           argv[0]);
    return 2;
  }
  std::vector<replay::Event> ev;
  if (!replay::read_events(argv[1], ev)) { std::fprintf(stderr, "cannot read %s\n", argv[1]); return 1; }

  replay::ReplayConfig cfg;
  bool naive = false;
  bool use_strategy = true;
  bool hot_cold = false;
  unsigned order_size = 32;
  for (int i = 2; i < argc; ++i) {
    if (std::strcmp(argv[i], "--latency") == 0) cfg.measure_latency = true;
    else if (std::strcmp(argv[i], "--naive") == 0) naive = true;
    else if (std::strcmp(argv[i], "--no-strategy") == 0) use_strategy = false;
    else if (std::strcmp(argv[i], "--hot-cold") == 0) hot_cold = true;
    else if (std::strcmp(argv[i], "--order-size") == 0 && i + 1 < argc) {
      order_size = static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 10));
      if (order_size != 48 && order_size != 64) {
        std::fprintf(stderr, "--order-size must be 48 or 64\n");
        return 2;
      }
    }
    else {
      std::fprintf(stderr, "unknown option: %s\n", argv[i]);
      return 2;
    }
  }
  if ((naive && (order_size != 32 || hot_cold)) || (hot_cold && order_size != 32) ||
      ((order_size != 32 || hot_cold) && use_strategy)) {
    std::fprintf(stderr, "storage variants are exclusive and require --no-strategy\n");
    return 2;
  }
  replay::VwapTracker vwap;
  const auto st = naive ? replay::replay_naive(ev, cfg)
                        : hot_cold ? replay::replay_hotcold(ev, cfg)
                        : order_size == 48 ? replay::replay_order48(ev, cfg)
                        : order_size == 64 ? replay::replay_order64(ev, cfg)
                                           : replay::replay(ev, cfg, use_strategy ? &vwap : nullptr);

  std::printf("events        : %llu\n", (unsigned long long)st.events);
  std::printf("trades        : %llu (volume %llu)\n", (unsigned long long)st.trades,
              (unsigned long long)st.volume);
  std::printf("rejected      : %llu   cancel misses: %llu   resting at end: %llu\n",
              (unsigned long long)st.rejected, (unsigned long long)st.cancel_misses,
              (unsigned long long)st.final_orders);
  std::printf("throughput    : %.2f M events/s (%.3f s)\n", st.events_per_sec() / 1e6, st.seconds);
  if (!naive && !hot_cold && order_size == 32 && use_strategy)
    std::printf("vwap / last   : %.3f / %lld\n", vwap.vwap(), (long long)vwap.last());
  else
    std::printf("strategy      : disabled\n");
  std::printf("book checksum : %016llx\n", (unsigned long long)st.checksum);
  if (cfg.measure_latency) {
    for (double p : {50.0, 90.0, 99.0, 99.9})
      std::printf("latency p%-5g : %llu ns\n", p, (unsigned long long)st.latency_percentile(p));
  }
}
