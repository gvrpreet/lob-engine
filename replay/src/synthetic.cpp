#include "replay/synthetic.hpp"

#include <random>
#include <limits>
#include <stdexcept>
#include <utility>

namespace replay {

std::vector<Event> generate_events(std::size_t count, std::uint64_t seed) {
  std::mt19937_64 rng(seed);
  std::vector<Event> out;
  out.reserve(count);
  std::vector<lob::OrderId> live;  // ids that *may* still rest (fills are not tracked on purpose)
  lob::OrderId next_id = 1;
  lob::Price mid = 10'000;
  lob::Timestamp ts = 1'000'000'000ULL;

  for (std::size_t i = 0; i < count; ++i) {
    ts += 500 + rng() % 1000;
    if (i % 100 == 0) mid += static_cast<lob::Price>(rng() % 3) - 1;  // random walk
    const auto r = static_cast<unsigned>(rng() % 100);

    Event e{};
    e.ts = ts;
    if (r < 20 && !live.empty()) {
      const std::size_t k = rng() % live.size();
      e.type = EventType::Cancel;
      e.id = live[k];
      live[k] = live.back();
      live.pop_back();
    } else {
      e.type = EventType::New;
      e.id = next_id++;
      e.side = (rng() & 1) ? lob::Side::Buy : lob::Side::Sell;
      e.qty = 1 + static_cast<lob::Qty>(rng() % 100);
      if (r < 30) {
        e.otype = lob::OrderType::Market;
      } else {
        e.otype = lob::OrderType::Limit;
        const lob::Price off = static_cast<lob::Price>(rng() % 21);
        // +/-2 tick overlap makes a fraction of limit orders cross the spread.
        e.price = e.side == lob::Side::Buy ? mid + 2 - off : mid - 2 + off;
        live.push_back(e.id);
      }
    }
    out.push_back(e);
  }
  return out;
}

std::vector<Event> generate_depth_events(std::size_t count, std::size_t active_levels,
                                         std::uint64_t seed,
                                         std::uint32_t market_trade_percent) {
  if (active_levels == 0 || count < active_levels || market_trade_percent > 100) {
    throw std::invalid_argument("event count must cover at least one anchor per level");
  }

  std::mt19937_64 rng(seed);
  std::vector<Event> out;
  out.reserve(count);
  std::vector<std::pair<lob::Side, lob::Price>> levels;
  levels.reserve(active_levels);

  const std::size_t bid_levels = active_levels / 2;
  const std::size_t ask_levels = active_levels - bid_levels;
  for (std::size_t i = 0; i < bid_levels; ++i) {
    levels.emplace_back(lob::Side::Buy, 9'999 - static_cast<lob::Price>(i));
  }
  for (std::size_t i = 0; i < ask_levels; ++i) {
    levels.emplace_back(lob::Side::Sell, 10'001 + static_cast<lob::Price>(i));
  }

  std::vector<lob::OrderId> live;
  live.reserve(count - active_levels);
  lob::OrderId next_id = 1;
  lob::Timestamp timestamp = 1'000'000'000ULL;

  for (const auto& [side, price] : levels) {
    Event event{};
    event.ts = ++timestamp;
    event.id = next_id++;
    event.price = price;
    event.qty = market_trade_percent == 0 ? 1 : std::numeric_limits<lob::Qty>::max();
    event.type = EventType::New;
    event.side = side;
    event.otype = lob::OrderType::Limit;
    out.push_back(event);
  }

  for (std::size_t i = active_levels; i < count; ++i) {
    Event event{};
    event.ts = ++timestamp;
    if (!live.empty() && rng() % 100 < 20) {
      const std::size_t index = rng() % live.size();
      event.type = EventType::Cancel;
      event.id = live[index];
      live[index] = live.back();
      live.pop_back();
    } else if (market_trade_percent != 0 && rng() % 100 < market_trade_percent) {
      event.type = EventType::New;
      event.id = next_id++;
      event.qty = 1 + static_cast<lob::Qty>(rng() % 100);
      event.side = (rng() & 1) ? lob::Side::Buy : lob::Side::Sell;
      event.otype = lob::OrderType::Market;
    } else {
      const auto& [side, price] = levels[rng() % levels.size()];
      event.type = EventType::New;
      event.id = next_id++;
      event.price = price;
      event.qty = 1 + static_cast<lob::Qty>(rng() % 100);
      event.side = side;
      event.otype = lob::OrderType::Limit;
      live.push_back(event.id);
    }
    out.push_back(event);
  }
  return out;
}

}  // namespace replay
