#pragma once
#include <cstdint>
#include <span>
#include <vector>

#include "lob/order_book.hpp"
#include "replay/event.hpp"
#include "replay/latency_histogram.hpp"
#include "replay/timer.hpp"

namespace replay {

// Hook for strategy evaluation. Called synchronously by the replayer.
class IStrategy {
 public:
  virtual ~IStrategy() = default;
  virtual void on_trade(const lob::Trade&) {}
  virtual void on_event(const Event&, const lob::OrderBook&) {}
};

struct ReplayConfig {
  std::size_t book_capacity = 1u << 20;
  bool measure_latency = false;  // per-event timing; each sample includes the timer's overhead
  LatencyTimer latency_timer = LatencyTimer::SteadyClock;  // Tsc: calibrated rdtscp
  bool measure_book_shape = false;  // collect final and peak active price-level counts
};

struct ReplayStats {
  std::uint64_t events = 0, trades = 0, volume = 0;
  std::uint64_t rejected = 0, cancel_misses = 0, final_orders = 0;
  std::uint64_t modify_misses = 0, reduce_misses = 0;  // v2 logs: the id was not resting
  std::uint64_t final_levels = 0, peak_levels = 0;
  std::uint64_t checksum = 0;  // book state hash at end of replay
  std::uint64_t anon_huge_kb = 0;  // process AnonHugePages at end of replay_hugepages (Linux only)
  double seconds = 0;
  LatencyHistogram latency;  // per-event ns, filled only if measure_latency

  double events_per_sec() const { return seconds > 0 ? static_cast<double>(events) / seconds : 0; }
  std::uint64_t latency_percentile(double p) const { return latency.percentile(p); }  // p in [0,100]
};

ReplayStats replay(std::span<const Event> events, const ReplayConfig& cfg = {},
                   IStrategy* strategy = nullptr);
ReplayStats replay_naive(std::span<const Event> events, const ReplayConfig& cfg = {});
ReplayStats replay_order48(std::span<const Event> events, const ReplayConfig& cfg = {});
ReplayStats replay_order64(std::span<const Event> events, const ReplayConfig& cfg = {});
ReplayStats replay_hotcold(std::span<const Event> events, const ReplayConfig& cfg = {});
ReplayStats replay_dense(std::span<const Event> events, const ReplayConfig& cfg = {});
ReplayStats replay_sorted(std::span<const Event> events, const ReplayConfig& cfg = {});
ReplayStats replay_hugepages(std::span<const Event> events, const ReplayConfig& cfg = {});

}  // namespace replay
