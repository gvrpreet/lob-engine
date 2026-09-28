#include "replay/trading.hpp"

#include <algorithm>
#include <chrono>
#include <unordered_map>
#include <vector>

namespace replay {

namespace {

// 2 x mid in ticks (exact), if both sides are quoted.
bool mid2_of(const lob::OrderBook& book, std::int64_t& mid2) {
  const auto bid = book.best_bid();
  const auto ask = book.best_ask();
  if (!bid || !ask) return false;
  mid2 = *bid + *ask;
  return true;
}

// What the replayer knows about each strategy order still resting (or being entered).
struct Live {
  lob::Side side;
  lob::Qty remaining;
  std::int64_t mid2;  // decision mid of the latest submit/modify
  bool has_mid;
};

class Accounting {
 public:
  explicit Accounting(StrategyMetrics& m) : m_(m) {}

  void fill(const Live& order, lob::Price price, lob::Qty qty, bool aggressor) {
    const auto q = static_cast<std::int64_t>(qty);
    const std::int64_t signed_q = order.side == lob::Side::Buy ? q : -q;
    ++m_.fills;
    if (aggressor) ++m_.aggressive_fills;
    m_.filled_qty += qty;
    m_.cash -= signed_q * price;

    // Average-cost realised PnL.
    const std::int64_t held = m_.position < 0 ? -m_.position : m_.position;
    if (m_.position == 0 || (m_.position > 0) == (signed_q > 0)) {
      avg_cost_ = (avg_cost_ * static_cast<double>(held) + static_cast<double>(price * q)) /
                  static_cast<double>(held + q);
    } else {
      const std::int64_t closing = std::min(q, held);
      const double per_share = m_.position > 0 ? static_cast<double>(price) - avg_cost_
                                               : avg_cost_ - static_cast<double>(price);
      m_.realised_pnl += per_share * static_cast<double>(closing);
      if (q > held) avg_cost_ = static_cast<double>(price);  // flipped: the rest opens here
    }
    m_.position += signed_q;

    if (order.has_mid) {
      const std::int64_t slip2 = order.side == lob::Side::Buy ? 2 * price - order.mid2
                                                              : order.mid2 - 2 * price;
      m_.slippage += static_cast<double>(slip2 * q) / 2.0;
      m_.slippage_qty += qty;
    }
  }

 private:
  StrategyMetrics& m_;
  double avg_cost_ = 0;
};

}  // namespace

void OrderGateway::push(Action a) {
  a.at = now_ + latency_;
  a.has_mid = mid2_of(book_, a.mid2);
  pending_.push_back(a);
}

lob::OrderId OrderGateway::submit(lob::Side side, lob::OrderType type, lob::Price price,
                                  lob::Qty qty) {
  const lob::OrderId id = next_id_++;
  push(Action{Kind::Submit, 0, id, side, type, price, qty, 0, false});
  return id;
}

void OrderGateway::cancel(lob::OrderId id) {
  push(Action{Kind::Cancel, 0, id, lob::Side::Buy, lob::OrderType::Limit, 0, 0, 0, false});
}

void OrderGateway::modify(lob::OrderId id, lob::Price price, lob::Qty qty) {
  push(Action{Kind::Modify, 0, id, lob::Side::Buy, lob::OrderType::Limit, price, qty, 0, false});
}

TradingStats replay_trading(std::span<const Event> events, const TradingConfig& cfg,
                            ITradingStrategy& strategy) {
  lob::OrderBook book(cfg.book_capacity);
  TradingStats out;
  ReplayStats& st = out.replay;
  StrategyMetrics& m = out.strategy;
  OrderGateway gw(book, cfg.order_latency_ns);
  Accounting accounting(m);
  std::unordered_map<lob::OrderId, Live> live;
  std::vector<Fill> fills;  // delivered after each book operation, when the book is consistent
  lob::Price last_trade = 0;
  bool traded = false;

  const auto record = [&](lob::OrderId id, const lob::Trade& t, bool aggressor) {
    auto it = live.find(id);
    if (it == live.end()) return;  // cannot happen: every resting strategy order is tracked
    accounting.fill(it->second, t.price, t.qty, aggressor);
    it->second.remaining -= t.qty;
    fills.push_back(Fill{id, it->second.side, t.price, t.qty, t.ts, it->second.remaining, aggressor});
    if (it->second.remaining == 0) live.erase(it);
  };
  const auto sink = [&](const lob::Trade& t) {
    ++st.trades;
    st.volume += t.qty;
    last_trade = t.price;
    traded = true;
    if (is_strategy_id(t.taker)) record(t.taker, t, true);
    if (is_strategy_id(t.maker)) record(t.maker, t, false);
  };
  const auto deliver = [&] {
    for (std::size_t i = 0; i < fills.size(); ++i) {  // on_fill may queue actions, not fills
      strategy.on_fill(fills[i], gw);
    }
    fills.clear();
  };

  const auto apply = [&](const OrderGateway::Action& a) {
    using Kind = OrderGateway::Kind;
    if (a.kind == Kind::Submit) {
      live.emplace(a.id, Live{a.side, a.qty, a.mid2, a.has_mid});  // ids are unique by construction
      const auto r = book.add(a.id, a.side, a.type, a.price, a.qty, a.at, sink);
      if (r == lob::AddResult::Rejected) {
        ++m.rejected;
        live.erase(a.id);
        return;
      }
      ++m.orders;
      m.submitted_qty += a.qty;
      if (r == lob::AddResult::Discarded) live.erase(a.id);  // market remainder never rests
    } else if (a.kind == Kind::Cancel) {
      if (book.cancel(a.id)) {
        ++m.cancels;
        live.erase(a.id);
      } else {
        ++m.misses;
      }
    } else {
      auto it = live.find(a.id);
      if (it == live.end()) {
        ++m.misses;
        return;
      }
      if (a.qty == 0) {
        ++m.rejected;  // the book would refuse it too; the order stays as it was
        return;
      }
      if (a.qty > it->second.remaining) m.submitted_qty += a.qty - it->second.remaining;
      it->second.remaining = a.qty;
      it->second.mid2 = a.mid2;
      it->second.has_mid = a.has_mid;
      book.modify(a.id, a.price, a.qty, a.at, sink);  // fills erase the record if it trades away
      ++m.modifies;
    }
  };
  const auto apply_due = [&](lob::Timestamp until) {
    while (!gw.pending_.empty() && gw.pending_.front().at <= until) {
      const OrderGateway::Action a = gw.pending_.front();
      gw.pending_.pop_front();
      gw.now_ = a.at;
      apply(a);
      deliver();
    }
  };

  const auto t0 = std::chrono::steady_clock::now();
  for (const Event& event : events) {
    apply_due(event.ts);
    gw.now_ = event.ts;
    if (is_strategy_id(event.id)) {
      ++st.rejected;  // feed ids must stay below kStrategyIdBase
    } else {
      switch (event.type) {
        case EventType::New:
          if (book.add(event.id, event.side, event.otype, event.price, event.qty, event.ts, sink) ==
              lob::AddResult::Rejected)
            ++st.rejected;
          break;
        case EventType::Cancel:
          if (!book.cancel(event.id)) ++st.cancel_misses;
          break;
        case EventType::Modify: {
          const auto r = book.modify(event.id, event.price, event.qty, event.ts, sink);
          if (r == lob::ModifyResult::NotFound) ++st.modify_misses;
          else if (r == lob::ModifyResult::Rejected) ++st.rejected;
          break;
        }
        case EventType::Reduce:
          if (!book.reduce(event.id, event.qty)) ++st.reduce_misses;
          break;
      }
    }
    deliver();
    strategy.on_event(event, book, gw);
    ++st.events;
  }
  st.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  m.dropped = gw.pending_.size();
  st.final_orders = book.order_count();
  st.checksum = book.checksum();

  std::int64_t mid2 = 0;
  if (mid2_of(book, mid2)) m.mark_price = static_cast<double>(mid2) / 2.0;
  else if (traded) m.mark_price = static_cast<double>(last_trade);
  m.marked_pnl = static_cast<double>(m.cash) + static_cast<double>(m.position) * m.mark_price;
  return out;
}

}  // namespace replay
