#include "replay/replayer.hpp"

#include <algorithm>
#include <chrono>
#include <type_traits>

namespace replay {

namespace {

// One instantiation per book with a runtime timer switch: instantiating the loop once per timer
// exhausted GCC's inlining budget and pushed map/index calls out of line in the unmeasured path.
template <class Book>
ReplayStats replay_with_book(std::span<const Event> events, const ReplayConfig& cfg,
                             IStrategy* strat) {
  using clock = std::chrono::steady_clock;
  Book book(cfg.book_capacity);
  ReplayStats st;
  const bool tsc = cfg.measure_latency && cfg.latency_timer == LatencyTimer::Tsc;
  const double ns_per_tick = tsc ? TscTimer::ns_per_unit() : 1.0;
  const auto read_timer = [tsc] { return tsc ? TscTimer::read() : SteadyTimer::read(); };

  const auto sink = [&](const lob::Trade& trade) {
    ++st.trades;
    st.volume += trade.qty;
    if (strat) strat->on_trade(trade);
  };

  const auto t0 = clock::now();
  for (const Event& event : events) {
    std::uint64_t start = 0;
    if (cfg.measure_latency) start = read_timer();

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
      const std::uint64_t elapsed = read_timer() - start;
      st.latency.record(tsc ? static_cast<std::uint64_t>(static_cast<double>(elapsed) * ns_per_tick)
                            : elapsed);
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
