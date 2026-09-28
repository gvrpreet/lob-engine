#include <cstdio>
#include <cstring>
#include <string>

#include "replay/event_log.hpp"
#include "replay/replayer.hpp"
#include "replay/strategies.hpp"
#include "replay/synthetic.hpp"
#include "testing.hpp"

TEST(generator_is_deterministic) {
  const auto a = replay::generate_events(10'000, 7);
  const auto b = replay::generate_events(10'000, 7);
  CHECK(a.size() == b.size());
  CHECK(std::memcmp(a.data(), b.data(), a.size() * sizeof(replay::Event)) == 0);
}

TEST(replay_is_deterministic) {
  const auto ev = replay::generate_events(200'000, 3);
  const auto s1 = replay::replay(ev);
  const auto s2 = replay::replay(ev);
  CHECK(s1.checksum == s2.checksum);
  CHECK(s1.trades == s2.trades && s1.volume == s2.volume);
  CHECK(s1.events == ev.size());
  CHECK(s1.trades > 0);
}

TEST(heap_order_baseline_matches_slab_replay) {
  const auto ev = replay::generate_events(100'000, 23);
  const auto slab = replay::replay(ev);
  const auto naive = replay::replay_naive(ev);
  CHECK(slab.checksum == naive.checksum);
  CHECK(slab.trades == naive.trades && slab.volume == naive.volume);
  CHECK(slab.rejected == naive.rejected && slab.cancel_misses == naive.cancel_misses);
  CHECK(slab.final_orders == naive.final_orders);
}

TEST(order_layout_variants_match_32_byte_replay) {
  const auto ev = replay::generate_events(100'000, 31);
  const auto baseline = replay::replay(ev);
  const auto padded = replay::replay_order48(ev);
  const auto cacheline = replay::replay_order64(ev);
  CHECK(baseline.checksum == padded.checksum && baseline.checksum == cacheline.checksum);
  CHECK(baseline.trades == padded.trades && baseline.trades == cacheline.trades);
  CHECK(baseline.volume == padded.volume && baseline.volume == cacheline.volume);
  CHECK(baseline.final_orders == padded.final_orders &&
        baseline.final_orders == cacheline.final_orders);
}

TEST(hot_cold_order_storage_matches_aos_replay) {
  const auto ev = replay::generate_events(100'000, 37);
  const auto aos = replay::replay(ev);
  const auto hotcold = replay::replay_hotcold(ev);
  CHECK(aos.checksum == hotcold.checksum);
  CHECK(aos.trades == hotcold.trades && aos.volume == hotcold.volume);
  CHECK(aos.rejected == hotcold.rejected && aos.cancel_misses == hotcold.cancel_misses);
  CHECK(aos.final_orders == hotcold.final_orders);
}

TEST(huge_page_storage_matches_default_replay) {
  const auto ev = replay::generate_events(100'000, 41);
  const auto base = replay::replay(ev);
  const auto hp = replay::replay_hugepages(ev);
  CHECK(base.checksum == hp.checksum);
  CHECK(base.trades == hp.trades && base.volume == hp.volume);
  CHECK(base.rejected == hp.rejected && base.cancel_misses == hp.cancel_misses);
  CHECK(base.final_orders == hp.final_orders);
  CHECK(base.anon_huge_kb == 0);  // only sampled by the huge-page variant
}

TEST(dense_price_levels_match_tree_replay) {
  const auto ev = replay::generate_events(150'000, 41);
  const auto tree = replay::replay_tree(ev);
  const auto dense = replay::replay(ev);  // dense levels are the default
  CHECK(tree.checksum == dense.checksum);
  CHECK(tree.trades == dense.trades && tree.volume == dense.volume);
  CHECK(tree.rejected == dense.rejected && tree.cancel_misses == dense.cancel_misses);
  CHECK(tree.final_orders == dense.final_orders);
}

TEST(sorted_price_levels_match_tree_replay) {
  const auto ev = replay::generate_events(150'000, 43);
  const auto tree = replay::replay_tree(ev);
  const auto sorted = replay::replay_sorted(ev);
  CHECK(tree.checksum == sorted.checksum);
  CHECK(tree.trades == sorted.trades && tree.volume == sorted.volume);
  CHECK(tree.rejected == sorted.rejected && tree.cancel_misses == sorted.cancel_misses);
  CHECK(tree.final_orders == sorted.final_orders);
}

TEST(book_shape_metrics_are_opt_in_and_match_across_level_stores) {
  const auto ev = replay::generate_events(50'000, 47);
  const auto default_stats = replay::replay(ev);
  replay::ReplayConfig cfg;
  cfg.measure_book_shape = true;
  const auto tree = replay::replay_tree(ev, cfg);
  const auto dense = replay::replay(ev, cfg);
  const auto sorted = replay::replay_sorted(ev, cfg);

  CHECK(default_stats.final_levels == 0 && default_stats.peak_levels == 0);
  CHECK(tree.final_levels > 0 && tree.peak_levels >= tree.final_levels);
  CHECK(tree.final_levels == dense.final_levels && tree.final_levels == sorted.final_levels);
  CHECK(tree.peak_levels == dense.peak_levels && tree.peak_levels == sorted.peak_levels);
}

TEST(controlled_depth_events_hold_requested_level_count) {
  for (const std::size_t depth : {10, 25, 50}) {
    const auto events = replay::generate_depth_events(20'000, depth, 53);
    const auto repeated = replay::generate_depth_events(20'000, depth, 53);
    CHECK(events.size() == repeated.size());
    CHECK(std::memcmp(events.data(), repeated.data(), events.size() * sizeof(replay::Event)) == 0);

    replay::ReplayConfig cfg;
    cfg.measure_book_shape = true;
    const auto tree = replay::replay_tree(events, cfg);
    const auto dense = replay::replay(events, cfg);
    const auto sorted = replay::replay_sorted(events, cfg);
    CHECK(tree.final_levels == depth && tree.peak_levels == depth);
    CHECK(tree.checksum == dense.checksum && tree.checksum == sorted.checksum);
    CHECK(tree.trades == 0 && tree.trades == dense.trades && tree.trades == sorted.trades);
    CHECK(tree.final_levels == dense.final_levels && tree.final_levels == sorted.final_levels);
  }
}

TEST(trade_enabled_controlled_depth_preserves_levels_across_backends) {
  for (const std::size_t depth : {10, 25, 50}) {
    const auto events = replay::generate_depth_events(50'000, depth, 59, 25);
    const auto repeated = replay::generate_depth_events(50'000, depth, 59, 25);
    CHECK(events.size() == repeated.size());
    CHECK(std::memcmp(events.data(), repeated.data(), events.size() * sizeof(replay::Event)) == 0);

    replay::ReplayConfig cfg;
    cfg.measure_book_shape = true;
    const auto tree = replay::replay_tree(events, cfg);
    const auto dense = replay::replay(events, cfg);
    const auto sorted = replay::replay_sorted(events, cfg);
    CHECK(tree.trades > 0 && tree.trades == dense.trades && tree.trades == sorted.trades);
    CHECK(tree.final_levels == depth && tree.peak_levels == depth);
    CHECK(tree.final_levels == dense.final_levels && tree.final_levels == sorted.final_levels);
    CHECK(tree.checksum == dense.checksum && tree.checksum == sorted.checksum);
  }
}

TEST(event_log_roundtrip) {
  const auto ev = replay::generate_events(5'000, 11);
  const std::string path = "/tmp/lob_test_events.bin";
  CHECK(replay::write_events(path, ev));
  std::vector<replay::Event> back;
  CHECK(replay::read_events(path, back));
  CHECK(back.size() == ev.size());
  CHECK(std::memcmp(ev.data(), back.data(), ev.size() * sizeof(replay::Event)) == 0);
  std::remove(path.c_str());
}

TEST(strategy_hook_sees_every_trade) {
  const auto ev = replay::generate_events(50'000, 5);
  replay::VwapTracker v;
  const auto st = replay::replay(ev, {}, &v);
  CHECK(st.trades > 0);
  CHECK(v.vwap() > 9'000 && v.vwap() < 11'000);
}
