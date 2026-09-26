#include "replay/replayer.hpp"

#include <algorithm>
#include <chrono>
#include <type_traits>

namespace replay {

namespace {

template <class Book>
ReplayStats replay_with_book(std::span<const Event> events, const ReplayConfig& cfg,
                             IStrategy* strat) {
  using clock = std::chrono::steady_clock;
  Book book(cfg.book_capacity);
  ReplayStats st;
  if (cfg.measure_latency) st.latency_ns.reserve(events.size());

  const auto sink = [&](const lob::Trade& trade) {
    ++st.trades;
    st.volume += trade.qty;
    if (strat) strat->on_trade(trade);
  };

  const auto t0 = clock::now();
  for (const Event& event : events) {
    clock::time_point start{};
    if (cfg.measure_latency) start = clock::now();

    if (event.type == EventType::New) {
      if (book.add(event.id, event.side, event.otype, event.price, event.qty, event.ts, sink) ==
          lob::AddResult::Rejected)
        ++st.rejected;
    } else if (!book.cancel(event.id)) {
      ++st.cancel_misses;
    }

    if (cfg.measure_book_shape) {
      const auto levels = book.level_count(lob::Side::Buy) + book.level_count(lob::Side::Sell);
      st.peak_levels = std::max(st.peak_levels, static_cast<std::uint64_t>(levels));
    }

    if (cfg.measure_latency) {
      st.latency_ns.push_back(static_cast<std::uint32_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - start).count()));
    }
    if constexpr (std::is_same_v<Book, lob::OrderBook>) {
      if (strat) strat->on_event(event, book);
    }
    ++st.events;
  }
  st.seconds = std::chrono::duration<double>(clock::now() - t0).count();
  st.final_orders = book.order_count();
  if (cfg.measure_book_shape) {
    st.final_levels = book.level_count(lob::Side::Buy) + book.level_count(lob::Side::Sell);
  }
  st.checksum = book.checksum();
  return st;
}

}  // namespace

std::uint64_t ReplayStats::latency_percentile(double p) const {
  if (latency_ns.empty()) return 0;
  std::vector<std::uint32_t> v = latency_ns;
  const auto k = static_cast<std::size_t>(p / 100.0 * static_cast<double>(v.size() - 1));
  std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), v.end());
  return v[k];
}

ReplayStats replay(std::span<const Event> events, const ReplayConfig& cfg, IStrategy* strat) {
  return replay_with_book<lob::OrderBook>(events, cfg, strat);
}

ReplayStats replay_naive(std::span<const Event> events, const ReplayConfig& cfg) {
  return replay_with_book<lob::NaiveOrderBook>(events, cfg, nullptr);
}

ReplayStats replay_order48(std::span<const Event> events, const ReplayConfig& cfg) {
  return replay_with_book<lob::OrderBook48>(events, cfg, nullptr);
}

ReplayStats replay_order64(std::span<const Event> events, const ReplayConfig& cfg) {
  return replay_with_book<lob::OrderBook64>(events, cfg, nullptr);
}

ReplayStats replay_hotcold(std::span<const Event> events, const ReplayConfig& cfg) {
  return replay_with_book<lob::HotColdOrderBook>(events, cfg, nullptr);
}

ReplayStats replay_dense(std::span<const Event> events, const ReplayConfig& cfg) {
  return replay_with_book<lob::DenseOrderBook>(events, cfg, nullptr);
}

ReplayStats replay_sorted(std::span<const Event> events, const ReplayConfig& cfg) {
  return replay_with_book<lob::SortedOrderBook>(events, cfg, nullptr);
}

}  // namespace replay
