#pragma once
#include <cstdint>
#include <vector>

#include "replay/event.hpp"

namespace replay {

// Deterministic synthetic exchange feed: random-walking mid price, ~70% limit,
// ~10% market, ~20% cancel. Same (count, seed) => byte-identical output.
std::vector<Event> generate_events(std::size_t count, std::uint64_t seed = 42);

}  // namespace replay
