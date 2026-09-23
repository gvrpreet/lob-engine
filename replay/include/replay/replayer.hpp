#pragma once
#include <cstdint>
#include <span>
#include <vector>

#include "lob/order_book.hpp"
#include "replay/event.hpp"

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
  bool measure_latency = false;  // per-event steady_clock timing (adds ~20-30ns overhead)
};

struct ReplayStats {
  std::uint64_t events = 0, trades = 0, volume = 0;
  std::uint64_t rejected = 0, cancel_misses = 0, final_orders = 0;
  std::uint64_t checksum = 0;  // book state hash at end of replay
  double seconds = 0;
  std::vector<std::uint32_t> latency_ns;  // filled only if measure_latency

  double events_per_sec() const { return seconds > 0 ? static_cast<double>(events) / seconds : 0; }
  std::uint64_t latency_percentile(double p) const;  // p in [0,100]
};

ReplayStats replay(std::span<const Event> events, const ReplayConfig& cfg = {},
                   IStrategy* strategy = nullptr);

}  // namespace replay
