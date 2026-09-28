// Phase 5(f)+(g): strategy order entry through the latency-delayed gateway, and its metrics.
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "replay/event_log.hpp"
#include "replay/replayer.hpp"
#include "replay/strategies.hpp"
#include "replay/trading.hpp"
#include "testing.hpp"

using namespace lob;
using replay::Event;
using replay::EventType;

namespace {

Event limit(Timestamp ts, OrderId id, Side side, Price price, Qty qty) {
  Event e{};
  e.ts = ts;
  e.id = id;
  e.side = side;
  e.price = price;
  e.qty = qty;
  e.type = EventType::New;
  e.otype = OrderType::Limit;
  return e;
}

Event market(Timestamp ts, OrderId id, Side side, Qty qty) {
  Event e = limit(ts, id, side, 0, qty);
  e.otype = OrderType::Market;
  return e;
}

// Runs a callback per feed event (by index) and records every fill it is told about.
struct Scripted final : replay::ITradingStrategy {
  std::function<void(std::size_t, const OrderBook&, replay::OrderGateway&)> step;
  std::vector<replay::Fill> fills;
  std::size_t index = 0;
  void on_event(const Event&, const OrderBook& book, replay::OrderGateway& gw) override {
    if (step) step(index, book, gw);
    ++index;
  }
  void on_fill(const replay::Fill& f, replay::OrderGateway&) override { fills.push_back(f); }
};

bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }

}  // namespace

TEST(gateway_actions_arrive_after_latency) {
  const std::vector<Event> ev{limit(100, 1, Side::Sell, 110, 5), limit(200, 2, Side::Buy, 90, 5),
                              limit(300, 3, Side::Buy, 80, 1), limit(400, 4, Side::Buy, 80, 1)};
  for (const Timestamp latency : {Timestamp{150}, Timestamp{100}}) {
    Scripted s;
    OrderId id = 0;
    std::vector<bool> resting;
    s.step = [&](std::size_t i, const OrderBook& book, replay::OrderGateway& gw) {
      if (i == 0) {
        CHECK(gw.now() == 100);
        id = gw.submit(Side::Buy, OrderType::Limit, 95, 2);
      }
      resting.push_back(book.contains(id));
    };
    replay::TradingConfig cfg;
    cfg.order_latency_ns = latency;
    const auto st = replay::replay_trading(ev, cfg, s);
    CHECK(replay::is_strategy_id(id) && id == replay::kStrategyIdBase);
    // Sent at ts 100. latency 150 arrives at 250: before the ts-300 event, not the ts-200 one.
    // latency 100 arrives at 200: applied before the ts-200 event (arrival <= event time).
    const bool at_200 = latency == 100;
    CHECK((resting == std::vector<bool>{false, at_200, true, true}));
    CHECK(st.strategy.orders == 1 && st.strategy.dropped == 0);
  }
}

TEST(fills_position_cash_pnl_and_slippage) {
  const std::vector<Event> ev{
      limit(100, 1, Side::Sell, 101, 10), limit(200, 2, Side::Buy, 99, 10),
      limit(300, 3, Side::Sell, 120, 1),  limit(400, 4, Side::Buy, 80, 1),
      market(500, 5, Side::Buy, 3),       limit(600, 6, Side::Sell, 130, 1)};
  Scripted s;
  s.step = [&](std::size_t i, const OrderBook&, replay::OrderGateway& gw) {
    if (i == 1) gw.submit(Side::Buy, OrderType::Limit, 101, 4);   // lifts the 101 offer
    if (i == 2) gw.submit(Side::Sell, OrderType::Limit, 99, 4);   // hits the 99 bid
    if (i == 3) gw.submit(Side::Sell, OrderType::Limit, 100, 3);  // rests; feed market buys it
  };
  const auto st = replay::replay_trading(ev, {}, s);
  const auto& m = st.strategy;
  const OrderId base = replay::kStrategyIdBase;

  CHECK(s.fills.size() == 3);
  CHECK(s.fills[0].id == base && s.fills[0].side == Side::Buy && s.fills[0].price == 101 &&
        s.fills[0].qty == 4 && s.fills[0].aggressor && s.fills[0].remaining == 0);
  CHECK(s.fills[1].id == base + 1 && s.fills[1].price == 99 && s.fills[1].aggressor);
  CHECK(s.fills[2].id == base + 2 && s.fills[2].price == 100 && !s.fills[2].aggressor &&
        s.fills[2].ts == 500);

  CHECK(m.orders == 3 && m.fills == 3 && m.aggressive_fills == 2);
  CHECK(m.submitted_qty == 11 && m.filled_qty == 11 && near(m.fill_rate(), 1.0));
  CHECK(m.position == -3);
  CHECK(m.cash == -404 + 396 + 300);
  CHECK(near(m.realised_pnl, -8.0));  // bought 4 @101, sold 4 @99; the short 3 @100 is still open
  CHECK(near(m.mark_price, 100.0));   // final book: 99 bid / 101 ask
  CHECK(near(m.marked_pnl, 292.0 - 300.0));
  // Mid at send was 100 each time: +1 paid on the buy, +1 on the hit bid, 0 on the passive sell.
  CHECK(near(m.slippage, 8.0) && m.slippage_qty == 11);
  CHECK(st.replay.trades == 3 && st.replay.events == 6);
}

TEST(cancel_modify_misses_rejects_and_in_flight) {
  std::vector<Event> ev{limit(100, 1, Side::Sell, 110, 5), limit(200, 2, Side::Buy, 90, 5)};
  for (OrderId id = 3; id <= 6; ++id) ev.push_back(limit(id * 100, id, Side::Buy, 50, 1));
  Scripted s;
  OrderId a = 0;
  bool requeued_at_96 = false;
  s.step = [&](std::size_t i, const OrderBook& book, replay::OrderGateway& gw) {
    if (i == 1) a = gw.submit(Side::Buy, OrderType::Limit, 95, 3);
    if (i == 2) gw.modify(a, 96, 3);
    if (i == 3) {
      requeued_at_96 = book.volume_at(Side::Buy, 96) == 3 && book.volume_at(Side::Buy, 95) == 0;
      gw.cancel(a);
    }
    if (i == 4) {
      gw.cancel(a);                                    // already gone: miss
      gw.modify(a, 97, 1);                             // miss
      gw.submit(Side::Buy, OrderType::Limit, 97, 0);  // zero qty: rejected by the book
    }
    if (i == 5) gw.submit(Side::Buy, OrderType::Limit, 97, 1);  // no later event: never arrives
  };
  const auto st = replay::replay_trading(ev, {}, s);
  const auto& m = st.strategy;
  CHECK(requeued_at_96);
  CHECK(m.orders == 1 && m.modifies == 1 && m.cancels == 1);
  CHECK(m.misses == 2 && m.rejected == 1 && m.dropped == 1);
  CHECK(m.submitted_qty == 3 && m.fills == 0 && m.position == 0);
}

TEST(queue_position_fills_from_feed_executions) {
  const auto execute = [](Timestamp ts, OrderId id, Qty qty) {
    Event e{};
    e.ts = ts;
    e.id = id;
    e.qty = qty;
    e.type = EventType::Execute;
    return e;
  };
  const std::vector<Event> ev{
      limit(100, 1, Side::Sell, 101, 10),
      limit(200, 2, Side::Buy, 99, 10),   // strategy bid (sent after this) queues behind it
      execute(300, 2, 4),                 // ahead of us: we only move up the queue
      limit(400, 3, Side::Buy, 99, 10),   // behind us
      execute(500, 3, 3),                 // behind us: the aggressor would have hit us first
      execute(600, 2, 6),                 // ahead of us (order 2 is now gone)
      limit(700, 4, Side::Buy, 98, 5),
      execute(800, 4, 5),                 // a worse price: the aggressor went through our 99
      limit(900, 5, Side::Buy, 97, 1)};
  for (const bool model : {true, false}) {
    Scripted s;
    s.step = [](std::size_t i, const OrderBook&, replay::OrderGateway& gw) {
      if (i == 1) gw.submit(Side::Buy, OrderType::Limit, 99, 5);
    };
    replay::TradingConfig cfg;
    cfg.queue_fills = model;
    const auto st = replay::replay_trading(ev, cfg, s);
    const auto& m = st.strategy;
    if (!model) {  // without the model nothing ever crosses the bid: no fills at all
      CHECK(m.fills == 0 && m.position == 0 && m.queue_fills == 0);
      continue;
    }
    CHECK(s.fills.size() == 2);
    if (s.fills.size() != 2) continue;
    CHECK(s.fills[0].qty == 3 && s.fills[0].price == 99 && s.fills[0].ts == 500 &&
          !s.fills[0].aggressor && s.fills[0].remaining == 2);
    CHECK(s.fills[1].qty == 2 && s.fills[1].price == 99 && s.fills[1].ts == 800 &&
          s.fills[1].remaining == 0);
    CHECK(m.queue_fills == 2 && m.queue_fill_qty == 5 && m.fills == 2 && m.position == 5);
    CHECK(m.cash == -5 * 99);
    // The feed book is still exact: every feed execution was applied in full.
    CHECK(st.replay.reduce_misses == 0 && st.replay.final_orders == 3);  // 1, 3 (7 left), 5
  }
}

TEST(market_maker_leaves_a_level_where_it_is_alone) {
  // The real bid at 100 is cancelled and a new one appears at 98. With exclude_own_orders the
  // market maker follows the others' touch (to the 90 filler bid, then 98); the original version
  // keeps "joining" its own lone 100 bid.
  const std::vector<Event> ev{limit(100, 1, Side::Sell, 105, 10), limit(200, 2, Side::Buy, 100, 10),
                              limit(300, 9, Side::Buy, 90, 1),    // neutral: the mm bid arrives
                              [] { Event e{}; e.ts = 400; e.id = 2; e.type = EventType::Cancel; return e; }(),
                              limit(500, 3, Side::Buy, 98, 10),   // the others' touch is now 98
                              limit(600, 4, Side::Buy, 90, 1),    // neutral: a re-quote arrives
                              limit(700, 5, Side::Buy, 90, 1)};
  struct Spy final : replay::ITradingStrategy {
    replay::MarketMaker mm;
    std::vector<Price> bid;  // the strategy's resting bid price at each event (0 = none)
    explicit Spy(replay::MarketMaker::Params p) : mm(p) {}
    void on_event(const Event& e, const OrderBook& book, replay::OrderGateway& gw) override {
      Price price = 0;
      for (OrderId id = replay::kStrategyIdBase; id < replay::kStrategyIdBase + 16; ++id) {
        Side s; Price p; Qty q;
        if (book.find_order(id, s, p, q) && s == Side::Buy) price = p;
      }
      bid.push_back(price);
      mm.on_event(e, book, gw);
    }
    void on_fill(const replay::Fill& f, replay::OrderGateway& gw) override { mm.on_fill(f, gw); }
  };
  for (const bool exclude : {true, false}) {
    replay::MarketMaker::Params p;
    p.exclude_own_orders = exclude;
    Spy spy(p);
    replay::replay_trading(ev, {}, spy);
    CHECK(spy.bid.size() == ev.size());
    if (spy.bid.size() != ev.size()) continue;
    CHECK(spy.bid[2] == 100 && spy.bid[3] == 100);  // joined the real bid at 100
    if (exclude) {
      // Alone at 100 after the cancel: steps down to the next real level (the 90 bid), then
      // up to 98 when that becomes the others' touch.
      CHECK(spy.bid[4] == 90 && spy.bid[5] == 98 && spy.bid[6] == 98);
    } else {
      CHECK(spy.bid[4] == 100 && spy.bid[5] == 100 && spy.bid[6] == 100);  // stuck on itself
    }
  }
}

TEST(feed_ids_in_strategy_range_are_skipped) {
  const std::vector<Event> ev{limit(100, replay::kStrategyIdBase + 5, Side::Buy, 90, 5),
                              limit(200, 1, Side::Sell, 110, 5)};
  Scripted s;
  const auto st = replay::replay_trading(ev, {}, s);
  CHECK(st.replay.rejected == 1 && st.replay.final_orders == 1);
}

TEST(silent_strategy_reproduces_plain_replay_on_golden_logs) {
  for (const char* name : {"/golden_2000_seed7.bin", "/golden_v2_2000_seed7.bin"}) {
    std::vector<Event> ev;
    CHECK(replay::read_events(std::string(LOB_TEST_DATA_DIR) + name, ev));
    Scripted s;
    const auto t = replay::replay_trading(ev, {}, s);
    const auto p = replay::replay(ev);
    CHECK(t.replay.checksum == p.checksum);
    CHECK(t.replay.trades == p.trades && t.replay.volume == p.volume);
    CHECK(t.replay.rejected == p.rejected && t.replay.cancel_misses == p.cancel_misses);
    CHECK(t.replay.modify_misses == p.modify_misses && t.replay.reduce_misses == p.reduce_misses);
    CHECK(t.replay.final_orders == p.final_orders && t.replay.events == p.events);
    CHECK(t.strategy.orders == 0 && t.strategy.fills == 0);
  }
}

// Pinned results (replay_cli golden_2000_seed7.bin --strategy mm --order-latency 1000 [--mm-self-touch]).
// A change here means strategy-visible behaviour changed: matching, gateway timing, or the market
// maker. The self-touch pin is the original (pre-2026-09-28) market maker, kept as a compatibility check.
TEST(market_maker_golden_result) {
  std::vector<Event> ev;
  CHECK(replay::read_events(std::string(LOB_TEST_DATA_DIR) + "/golden_2000_seed7.bin", ev));
  replay::TradingConfig cfg;
  cfg.order_latency_ns = 1000;
  replay::MarketMaker mm;  // quotes at the others' touch (default)
  const auto r = replay::replay_trading(ev, cfg, mm);
  const auto& m = r.strategy;
  CHECK(r.replay.checksum == 0x94793a19cdfa4789ULL);
  CHECK(r.replay.trades == 680 && r.replay.volume == 14'637 && r.replay.final_orders == 696);
  CHECK(m.orders == 128 && m.modifies == 124 && m.cancels == 4 && m.misses == 2 && m.dropped == 0);
  CHECK(m.fills == 140 && m.aggressive_fills == 1);
  CHECK(m.filled_qty == 1257 && m.submitted_qty == 1317);
  CHECK(m.position == 15 && m.cash == -149'431);
  CHECK(near(m.mark_price, 9994.5));

  replay::MarketMaker::Params self_touch;
  self_touch.exclude_own_orders = false;
  replay::MarketMaker old(self_touch);
  const auto o = replay::replay_trading(ev, cfg, old);
  CHECK(o.replay.checksum == 0x75d40b3788c04ed5ULL);
  CHECK(o.replay.trades == 687 && o.replay.volume == 14'644 && o.replay.final_orders == 699);
  CHECK(o.strategy.orders == 135 && o.strategy.modifies == 100 && o.strategy.misses == 1);
  CHECK(o.strategy.fills == 151 && o.strategy.filled_qty == 1337 && o.strategy.submitted_qty == 1357);
  CHECK(o.strategy.position == 53 && o.strategy.cash == -529'268);
}

TEST(market_maker_is_deterministic_and_respects_position_limit) {
  std::vector<Event> ev;
  CHECK(replay::read_events(std::string(LOB_TEST_DATA_DIR) + "/golden_2000_seed7.bin", ev));
  replay::MarketMaker::Params p;
  p.size = 5;
  p.max_position = 20;
  for (const Timestamp latency : {Timestamp{0}, Timestamp{2000}}) {
    replay::TradingConfig cfg;
    cfg.order_latency_ns = latency;
    replay::MarketMaker a(p), b(p);
    const auto ra = replay::replay_trading(ev, cfg, a);
    const auto rb = replay::replay_trading(ev, cfg, b);
    const auto& m = ra.strategy;
    CHECK(ra.replay.checksum == rb.replay.checksum);
    CHECK(m.fills == rb.strategy.fills && m.cash == rb.strategy.cash &&
          m.position == rb.strategy.position && m.slippage == rb.strategy.slippage);
    CHECK(m.orders > 0 && m.fills > 0);
    CHECK(m.position == a.position());  // strategy's own view agrees with the replayer's
    // One live order per side plus at most one in-flight cancelled one can fill past the limit.
    CHECK(m.position <= p.max_position + 2 * p.size && m.position >= -(p.max_position + 2 * p.size));
    CHECK(near(m.marked_pnl, static_cast<double>(m.cash) + static_cast<double>(m.position) * m.mark_price));
    CHECK(m.filled_qty <= m.submitted_qty);
  }
}
