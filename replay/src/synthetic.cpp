#include "replay/synthetic.hpp"

#include <random>

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

}  // namespace replay
