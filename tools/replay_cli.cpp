// usage: replay_cli <events.bin> [--read] [--latency] [--tsc] [--huge-pages] [--naive] [--no-strategy] [--order-size 48|64] [--hot-cold] [--dense] [--sorted]
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include "replay/event_log.hpp"
#include "replay/mapped_event_log.hpp"
#include "replay/replayer.hpp"
#include "replay/strategies.hpp"

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr,
                 "usage: %s <events.bin> [--read] [--latency] [--tsc] [--huge-pages] [--naive] [--no-strategy] [--order-size 48|64] [--hot-cold] [--dense] [--sorted]\n",
           argv[0]);
    return 2;
  }
  // Default: memory-map the log (zero-copy). --read copies it into a vector instead (the old path).
  bool copy_read = false;
  for (int i = 2; i < argc; ++i) copy_read = copy_read || std::strcmp(argv[i], "--read") == 0;
  std::vector<replay::Event> owned;
  replay::MappedEventLog mapped;
  const auto load_start = std::chrono::steady_clock::now();
  if (std::string error; copy_read ? !replay::read_events(argv[1], owned, &error)
                                   : !mapped.open(argv[1], &error)) {
    std::fprintf(stderr, "cannot read %s: %s\n", argv[1], error.c_str());
    return 1;
  }
  const double load_ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - load_start).count();
  const std::span<const replay::Event> ev = copy_read ? std::span<const replay::Event>(owned) : mapped.events();

  replay::ReplayConfig cfg;
  bool naive = false;
  bool use_strategy = true;
  bool hot_cold = false;
  bool dense = false;
  bool sorted = false;
  bool huge = false;
  unsigned order_size = 32;
  for (int i = 2; i < argc; ++i) {
    if (std::strcmp(argv[i], "--read") == 0) continue;  // handled above
    else if (std::strcmp(argv[i], "--latency") == 0) cfg.measure_latency = true;
    else if (std::strcmp(argv[i], "--tsc") == 0) cfg.latency_timer = replay::LatencyTimer::Tsc;
    else if (std::strcmp(argv[i], "--huge-pages") == 0) huge = true;
    else if (std::strcmp(argv[i], "--naive") == 0) naive = true;
    else if (std::strcmp(argv[i], "--no-strategy") == 0) use_strategy = false;
    else if (std::strcmp(argv[i], "--hot-cold") == 0) hot_cold = true;
    else if (std::strcmp(argv[i], "--dense") == 0) dense = true;
    else if (std::strcmp(argv[i], "--sorted") == 0) sorted = true;
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
  const int variants =
      int{naive} + int{hot_cold} + int{order_size != 32} + int{dense} + int{sorted} + int{huge};
  if (variants > 1 || (variants == 1 && !naive && use_strategy)) {
    std::fprintf(stderr, "storage variants are exclusive and require --no-strategy\n");
    return 2;
  }
  replay::VwapTracker vwap;
  const auto st = naive ? replay::replay_naive(ev, cfg)
                        : hot_cold ? replay::replay_hotcold(ev, cfg)
                        : dense ? replay::replay_dense(ev, cfg)
                        : sorted ? replay::replay_sorted(ev, cfg)
                        : huge ? replay::replay_hugepages(ev, cfg)
                        : order_size == 48 ? replay::replay_order48(ev, cfg)
                        : order_size == 64 ? replay::replay_order64(ev, cfg)
                                           : replay::replay(ev, cfg, use_strategy ? &vwap : nullptr);

  std::printf("load          : %.1f ms (%s, validated)\n", load_ms, copy_read ? "read into vector" : "mmap");
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
  if (huge)
    std::printf("huge pages    : requested; process AnonHugePages %llu kB at end of replay\n",
                (unsigned long long)st.anon_huge_kb);
  if (cfg.measure_latency) {
    std::printf("latency timer : %s (empty-interval p50 %llu ns, included below)\n",
                replay::timer_name(cfg.latency_timer),
                (unsigned long long)replay::timer_overhead_ns(cfg.latency_timer));
    for (double p : {50.0, 90.0, 99.0, 99.9})
      std::printf("latency p%-5g : %llu ns\n", p, (unsigned long long)st.latency_percentile(p));
  }
}
