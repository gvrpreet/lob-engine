#pragma once
// Phase 5(e): timestamp-paced replay. Separate from replay() so the as-fast-as-possible loop
// is untouched; the default book only.
#include <cstdint>
#include <span>

#include "replay/event.hpp"
#include "replay/latency_histogram.hpp"
#include "replay/replayer.hpp"

namespace replay {

struct PacedStats {
  ReplayStats replay;  // identical to replay() on the same events (pacing changes only timing)
  double speed = 1;    // log time / wall time
  double log_span_s = 0;  // last minus first timestamp
  // How far behind its scheduled wall time each event was applied, in ns. It is measured
  // against the schedule, not against the previous event, so a backlog shows up as growing
  // lateness instead of being hidden (coordinated omission).
  LatencyHistogram lateness;
  std::uint64_t late_over_1ms = 0;
};

// Event i is due at wall_start + (ts_i - ts_0) / speed; it is applied at or after that time.
// Timestamps earlier than the first event's count as due immediately. `speed` must be > 0.
PacedStats replay_paced(std::span<const Event> events, const ReplayConfig& cfg, double speed,
                        IStrategy* strategy = nullptr);

}  // namespace replay
