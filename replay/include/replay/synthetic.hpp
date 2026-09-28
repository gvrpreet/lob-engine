#pragma once
#include <cstdint>
#include <vector>

#include "replay/event.hpp"

namespace replay {

// Deterministic synthetic exchange feed: random-walking mid price, ~70% limit,
// ~10% market, ~20% cancel. Same (count, seed) => byte-identical output.
// modify_percent > 0 (v2 logs) first turns that share of events into Modify/Reduce of a
// possibly-resting order: 1/3 reduce, 1/3 same-price new qty, 1/3 new price.
std::vector<Event> generate_events(std::size_t count, std::uint64_t seed = 42,
                                   std::uint32_t modify_percent = 0);
std::vector<Event> generate_depth_events(std::size_t count, std::size_t active_levels,
																				 std::uint64_t seed = 42,
																				 std::uint32_t market_trade_percent = 0);

}  // namespace replay
