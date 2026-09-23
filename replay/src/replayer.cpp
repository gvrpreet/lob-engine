#include "replay/replayer.hpp"

#include <algorithm>
#include <chrono>

namespace replay {

std::uint64_t ReplayStats::latency_percentile(double p) const {
  if (latency_ns.empty()) return 0;
  std::vector<std::uint32_t> v = latency_ns;
  const auto k = static_cast<std::size_t>(p / 100.0 * static_cast<double>(v.size() - 1));
  std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), v.end());
  return v[k];
}

ReplayStats replay(std::span<const Event> events, const ReplayConfig& cfg, IStrategy* strat) {
  using clock = std::chrono::steady_clock;
  lob::OrderBook book(cfg.book_capacity);
  ReplayStats st;
  if (cfg.measure_latency) st.latency_ns.reserve(events.size());

  const auto sink = [&](const lob::Trade& t) {
    ++st.trades;
    st.volume += t.qty;
    if (strat) strat->on_trade(t);
  };

  const auto t0 = clock::now();
  for (const Event& e : events) {
    clock::time_point a{};
    if (cfg.measure_latency) a = clock::now();

    if (e.type == EventType::New) {
      if (book.add(e.id, e.side, e.otype, e.price, e.qty, e.ts, sink) == lob::AddResult::Rejected)
        ++st.rejected;
    } else if (!book.cancel(e.id)) {
      ++st.cancel_misses;
    }

    if (cfg.measure_latency) {
      st.latency_ns.push_back(static_cast<std::uint32_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - a).count()));
    }
    if (strat) strat->on_event(e, book);
    ++st.events;
  }
  st.seconds = std::chrono::duration<double>(clock::now() - t0).count();
  st.final_orders = book.order_count();
  st.checksum = book.checksum();
  return st;
}

}  // namespace replay
