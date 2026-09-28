#include "replay/paced.hpp"

#include <chrono>
#include <stdexcept>
#include <thread>

#include "amend.hpp"

namespace replay {

PacedStats replay_paced(std::span<const Event> events, const ReplayConfig& cfg, double speed,
                        IStrategy* strat) {
  if (!(speed > 0)) throw std::invalid_argument("pacing speed must be > 0");
  using clock = std::chrono::steady_clock;
  PacedStats out;
  out.speed = speed;
  ReplayStats& st = out.replay;
  lob::OrderBook book(cfg.book_capacity);
  const auto sink = [&](const lob::Trade& trade) {
    ++st.trades;
    st.volume += trade.qty;
    if (strat) strat->on_trade(trade);
  };
  if (events.empty()) return out;

  const lob::Timestamp ts0 = events.front().ts;
  out.log_span_s = static_cast<double>(events.back().ts > ts0 ? events.back().ts - ts0 : 0) / 1e9;
  constexpr auto kSleepThreshold = std::chrono::microseconds(200);
  constexpr auto kWakeEarly = std::chrono::microseconds(100);

  const auto wall0 = clock::now();
  for (const Event& event : events) {
    const lob::Timestamp offset = event.ts > ts0 ? event.ts - ts0 : 0;
    const auto due = wall0 + std::chrono::nanoseconds(static_cast<std::int64_t>(
                                 static_cast<double>(offset) / speed));
    auto now = clock::now();
    if (now < due) {
      if (due - now > kSleepThreshold) std::this_thread::sleep_until(due - kWakeEarly);
      while ((now = clock::now()) < due) {
      }  // spin the last stretch: sleep wake-ups are too coarse
    }
    const auto late = std::chrono::duration_cast<std::chrono::nanoseconds>(now - due).count();
    out.lateness.record(static_cast<std::uint64_t>(late));
    if (late > 1'000'000) ++out.late_over_1ms;

    switch (event.type) {
      case EventType::New:
        if (book.add(event.id, event.side, event.otype, event.price, event.qty, event.ts, sink) ==
            lob::AddResult::Rejected)
          ++st.rejected;
        break;
      case EventType::Cancel:
        if (!book.cancel(event.id)) ++st.cancel_misses;
        break;
      case EventType::Modify:
      case EventType::Reduce:
        detail::apply_amend(book, event, st, strat);
        break;
    }
    if (strat) strat->on_event(event, book);
    ++st.events;
  }
  st.seconds = std::chrono::duration<double>(clock::now() - wall0).count();
  st.final_orders = book.order_count();
  st.checksum = book.checksum();
  return out;
}

}  // namespace replay
