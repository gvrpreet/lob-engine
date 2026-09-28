#pragma once
// Internal to lob_replay: the Modify/Reduce path of the replay loop.
#include "replay/event.hpp"
#include "replay/replayer.hpp"

namespace replay::detail {

// Applies a Modify or Reduce event, counting trades and misses into `st` exactly as the replay
// loop does. Defined in amend.cpp, a separate translation unit, on purpose. Instantiated next
// to the loop, modify/reduce would give add/cancel and the index/level helpers second call
// sites, and GCC would stop inlining them into the loop (+19% instructions/event on v1 logs;
// see docs/learning/experiments/2026-09-28-phase5c-modify.md). The loop's own sink must also
// stay a local lambda: add<lambda> has internal linkage, which GCC's called-once inlining needs.
template <class Book>
void apply_amend(Book& book, const Event& event, ReplayStats& st, IStrategy* strat);

}  // namespace replay::detail
