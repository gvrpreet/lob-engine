// usage: replay_cli <events.bin> [--read] [--latency] [--tsc] [--huge-pages] [--naive] [--no-strategy] [--order-size 48|64] [--hot-cold] [--tree] [--sorted]
//        (the default book uses dense levels; --tree selects the std::map level store)
//        replay_cli <events.bin> [--read] --strategy mm [--order-latency <ns>] [--no-queue-fills]
//                                                                                (trading replay, default book)
//        replay_cli <events.bin> --stream [--window <events>] [--no-strategy]    (bounded memory, default book)
//        replay_cli <events.bin> [--read] --paced <speed> [--no-strategy]        (timestamp-paced, default book)
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include "replay/event_log.hpp"
#include "replay/event_stream.hpp"
#include "replay/mapped_event_log.hpp"
#include "replay/paced.hpp"
#include "replay/replayer.hpp"
#include "replay/strategies.hpp"
#include "replay/trading.hpp"

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr,
                 "usage: %s <events.bin> [--read] [--latency] [--tsc] [--huge-pages] [--naive] [--no-strategy] [--order-size 48|64] [--hot-cold] [--tree] [--sorted]\n"
                 "       %s <events.bin> [--read] --strategy mm [--order-latency <ns>] [--no-queue-fills]\n"
                 "       %s <events.bin> --stream [--window <events>] [--no-strategy]\n"
                 "       %s <events.bin> [--read] --paced <speed> [--no-strategy]\n",
           argv[0], argv[0], argv[0], argv[0]);
    return 2;
  }
  // Streaming: never loads the whole log; memory is the book plus a window of records.
  bool stream = false;
  for (int i = 2; i < argc; ++i) stream = stream || std::strcmp(argv[i], "--stream") == 0;
  if (stream) {
    std::size_t window = std::size_t{1} << 20;
    bool with_strategy = true;
    for (int i = 2; i < argc; ++i) {
      if (std::strcmp(argv[i], "--stream") == 0) continue;
      if (std::strcmp(argv[i], "--window") == 0 && i + 1 < argc) {
        window = std::strtoull(argv[++i], nullptr, 10);
      } else if (std::strcmp(argv[i], "--no-strategy") == 0) {
        with_strategy = false;
      } else {
        std::fprintf(stderr, "--stream supports only --window <events> and --no-strategy\n");
        return 2;
      }
    }
    if (window == 0) {
      std::fprintf(stderr, "--window must be at least 1\n");
      return 2;
    }
    replay::EventLogStream in(window);
    replay::VwapTracker vwap;
    replay::ReplayStats st;
    std::string error;
    if (!in.open(argv[1], &error) ||
        !replay::replay_stream(in, {}, st, with_strategy ? &vwap : nullptr, &error)) {
      std::fprintf(stderr, "cannot replay %s: %s (after %llu events)\n", argv[1], error.c_str(),
                   (unsigned long long)st.events);
      return 1;
    }
    std::printf("load          : streamed (window %zu events = %.1f MB, v%u log)\n", window,
                static_cast<double>(window * sizeof(replay::Event)) / 1e6, in.version());
    std::printf("events        : %llu\n", (unsigned long long)st.events);
    std::printf("trades        : %llu (volume %llu)\n", (unsigned long long)st.trades,
                (unsigned long long)st.volume);
    std::printf("rejected      : %llu   cancel misses: %llu   resting at end: %llu\n",
                (unsigned long long)st.rejected, (unsigned long long)st.cancel_misses,
                (unsigned long long)st.final_orders);
    std::printf("amend misses  : modify %llu   reduce %llu\n", (unsigned long long)st.modify_misses,
                (unsigned long long)st.reduce_misses);
    std::printf("throughput    : %.2f M events/s (%.3f s, including reads)\n",
                st.events_per_sec() / 1e6, st.seconds);
    if (with_strategy)
      std::printf("vwap / last   : %.3f / %lld\n", vwap.vwap(), (long long)vwap.last());
    else
      std::printf("strategy      : disabled\n");
    std::printf("book checksum : %016llx\n", (unsigned long long)st.checksum);
    return 0;
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
  bool tree = false;  // std::map level store (dense levels are the default)
  bool sorted = false;
  bool huge = false;
  unsigned order_size = 32;
  bool market_maker = false;
  bool latency_given = false;
  double paced_speed = 0;  // > 0: timestamp-paced replay
  replay::TradingConfig trading;
  for (int i = 2; i < argc; ++i) {
    if (std::strcmp(argv[i], "--read") == 0) continue;  // handled above
    else if (std::strcmp(argv[i], "--paced") == 0 && i + 1 < argc) {
      paced_speed = std::strtod(argv[++i], nullptr);
      if (!(paced_speed > 0)) {
        std::fprintf(stderr, "--paced needs a speed > 0 (1 = real time)\n");
        return 2;
      }
    }
    else if (std::strcmp(argv[i], "--strategy") == 0 && i + 1 < argc) {
      if (std::strcmp(argv[++i], "mm") != 0) {
        std::fprintf(stderr, "--strategy supports: mm\n");
        return 2;
      }
      market_maker = true;
    }
    else if (std::strcmp(argv[i], "--order-latency") == 0 && i + 1 < argc) {
      trading.order_latency_ns = std::strtoull(argv[++i], nullptr, 10);
      latency_given = true;
    }
    else if (std::strcmp(argv[i], "--no-queue-fills") == 0) {
      trading.queue_fills = false;
      latency_given = true;  // also a trading-replay-only option
    }
    else if (std::strcmp(argv[i], "--latency") == 0) cfg.measure_latency = true;
    else if (std::strcmp(argv[i], "--tsc") == 0) cfg.latency_timer = replay::LatencyTimer::Tsc;
    else if (std::strcmp(argv[i], "--huge-pages") == 0) huge = true;
    else if (std::strcmp(argv[i], "--naive") == 0) naive = true;
    else if (std::strcmp(argv[i], "--no-strategy") == 0) use_strategy = false;
    else if (std::strcmp(argv[i], "--hot-cold") == 0) hot_cold = true;
    else if (std::strcmp(argv[i], "--tree") == 0) tree = true;
    else if (std::strcmp(argv[i], "--dense") == 0) {
      std::fprintf(stderr, "--dense: dense levels are the default now; use --tree for the std::map store\n");
      return 2;
    }
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
      int{naive} + int{hot_cold} + int{order_size != 32} + int{tree} + int{sorted} + int{huge};
  if (latency_given && !market_maker) {
    std::fprintf(stderr, "--order-latency / --no-queue-fills need --strategy mm\n");
    return 2;
  }
  if (paced_speed > 0) {
    if (variants > 0 || market_maker || cfg.measure_latency) {
      std::fprintf(stderr, "--paced runs on the default book without --latency or --strategy\n");
      return 2;
    }
    replay::VwapTracker vwap;
    const auto p = replay::replay_paced(ev, cfg, paced_speed, use_strategy ? &vwap : nullptr);
    const auto& st = p.replay;
    std::printf("load          : %.1f ms (%s, validated)\n", load_ms, copy_read ? "read into vector" : "mmap");
    std::printf("events        : %llu\n", (unsigned long long)st.events);
    std::printf("trades        : %llu (volume %llu)\n", (unsigned long long)st.trades,
                (unsigned long long)st.volume);
    std::printf("rejected      : %llu   cancel misses: %llu   resting at end: %llu\n",
                (unsigned long long)st.rejected, (unsigned long long)st.cancel_misses,
                (unsigned long long)st.final_orders);
    std::printf("amend misses  : modify %llu   reduce %llu\n", (unsigned long long)st.modify_misses,
                (unsigned long long)st.reduce_misses);
    std::printf("paced         : speed %g, log span %.3f s -> wall %.3f s\n", p.speed, p.log_span_s,
                st.seconds);
    std::printf("lateness      : p50 %llu / p99 %llu / p99.9 %llu / max %llu ns; %llu events > 1 ms late\n",
                (unsigned long long)p.lateness.percentile(50), (unsigned long long)p.lateness.percentile(99),
                (unsigned long long)p.lateness.percentile(99.9), (unsigned long long)p.lateness.max(),
                (unsigned long long)p.late_over_1ms);
    if (use_strategy)
      std::printf("vwap / last   : %.3f / %lld\n", vwap.vwap(), (long long)vwap.last());
    else
      std::printf("strategy      : disabled\n");
    std::printf("book checksum : %016llx\n", (unsigned long long)st.checksum);
    return 0;
  }
  if (market_maker) {
    if (variants > 0 || !use_strategy || cfg.measure_latency) {
      std::fprintf(stderr, "--strategy mm runs on the default book without --latency/--no-strategy\n");
      return 2;
    }
    replay::MarketMaker mm;
    const auto ts = replay::replay_trading(ev, trading, mm);
    const auto& st = ts.replay;
    const auto& m = ts.strategy;
    std::printf("load          : %.1f ms (%s, validated)\n", load_ms, copy_read ? "read into vector" : "mmap");
    std::printf("events        : %llu\n", (unsigned long long)st.events);
    std::printf("trades        : %llu (volume %llu)\n", (unsigned long long)st.trades,
                (unsigned long long)st.volume);
    std::printf("rejected      : %llu   cancel misses: %llu   resting at end: %llu\n",
                (unsigned long long)st.rejected, (unsigned long long)st.cancel_misses,
                (unsigned long long)st.final_orders);
    std::printf("amend misses  : modify %llu   reduce %llu\n", (unsigned long long)st.modify_misses,
                (unsigned long long)st.reduce_misses);
    std::printf("throughput    : %.2f M events/s (%.3f s)\n", st.events_per_sec() / 1e6, st.seconds);
    std::printf("strategy      : market maker, order latency %llu ns, queue-position fills %s\n",
                (unsigned long long)trading.order_latency_ns, trading.queue_fills ? "on" : "off");
    std::printf("mm orders     : %llu submitted, %llu modifies, %llu cancels, %llu rejected, %llu misses, %llu in flight at end\n",
                (unsigned long long)m.orders, (unsigned long long)m.modifies,
                (unsigned long long)m.cancels, (unsigned long long)m.rejected,
                (unsigned long long)m.misses, (unsigned long long)m.dropped);
    std::printf("mm fills      : %llu (%llu aggressive), %llu of %llu shares, fill rate %.4f\n",
                (unsigned long long)m.fills, (unsigned long long)m.aggressive_fills,
                (unsigned long long)m.filled_qty, (unsigned long long)m.submitted_qty, m.fill_rate());
    std::printf("mm queue fills: %llu (%llu shares) from feed executions behind or below the quote\n",
                (unsigned long long)m.queue_fills, (unsigned long long)m.queue_fill_qty);
    std::printf("mm position   : %lld   cash %lld ticks\n", (long long)m.position, (long long)m.cash);
    std::printf("mm pnl        : realised %.1f   marked %.1f (mark %.1f) ticks x shares\n",
                m.realised_pnl, m.marked_pnl, m.mark_price);
    std::printf("mm slippage   : %.4f ticks/share over %llu shares (vs mid at send; + = cost)\n",
                m.avg_slippage(), (unsigned long long)m.slippage_qty);
    std::printf("book checksum : %016llx\n", (unsigned long long)st.checksum);
    return 0;
  }
  if (variants > 1 || (variants == 1 && !naive && use_strategy)) {
    std::fprintf(stderr, "storage variants are exclusive and require --no-strategy\n");
    return 2;
  }
  replay::VwapTracker vwap;
  const auto st = naive ? replay::replay_naive(ev, cfg)
                        : hot_cold ? replay::replay_hotcold(ev, cfg)
                        : tree ? replay::replay_tree(ev, cfg)
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
  std::printf("amend misses  : modify %llu   reduce %llu\n", (unsigned long long)st.modify_misses,
              (unsigned long long)st.reduce_misses);
  std::printf("throughput   : %.2f M events/s (%.3f s)\n", st.events_per_sec() / 1e6, st.seconds);
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
