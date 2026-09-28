#pragma once
// Phase 5(f)+(g): strategies that send orders into the replayed book, and their metrics.
// This is a separate replay entry from replay(): the plain replay loop is not touched, so its
// inlining and instruction count cannot change (see the Phase 5(c) note on why that matters).
#include <cstdint>
#include <deque>
#include <span>

#include "lob/order_book.hpp"
#include "replay/event.hpp"
#include "replay/replayer.hpp"

namespace replay {

// Strategy order ids live in the top half of the id space. Feed events with ids in that range
// are skipped (counted as rejected) so the two can never collide.
inline constexpr lob::OrderId kStrategyIdBase = lob::OrderId{1} << 63;
inline constexpr bool is_strategy_id(lob::OrderId id) noexcept { return id >= kStrategyIdBase; }

struct Fill {
  lob::OrderId id;  // the strategy's order
  lob::Side side;   // that order's side
  lob::Price price;
  lob::Qty qty;
  lob::Timestamp ts;
  lob::Qty remaining;  // the order's shares still in the book after this fill (0 = gone)
  bool aggressor;      // the strategy order was the taker
};

struct TradingConfig {
  std::size_t book_capacity = 1u << 20;
  // Every gateway action reaches the book this long after it was sent. It is applied before the
  // first feed event whose timestamp is >= its arrival time; actions keep their send order.
  lob::Timestamp order_latency_ns = 0;
};

class OrderGateway;
class ITradingStrategy;

struct StrategyMetrics {
  std::uint64_t orders = 0;         // submits accepted by the book
  std::uint64_t cancels = 0;        // cancels that removed a resting order
  std::uint64_t modifies = 0;       // modifies applied to a resting order
  std::uint64_t rejected = 0;       // submits/modifies the book refused (e.g. zero qty)
  std::uint64_t misses = 0;         // cancel/modify of an order that was no longer resting
  std::uint64_t dropped = 0;        // actions still in flight when the log ended
  std::uint64_t submitted_qty = 0;  // shares offered: submit qty plus any qty raised by modify
  std::uint64_t filled_qty = 0, fills = 0, aggressive_fills = 0;
  std::int64_t position = 0;        // shares, positive = long
  std::int64_t cash = 0;            // ticks x shares: sells add, buys subtract
  double realised_pnl = 0;          // ticks x shares, average-cost method
  double mark_price = 0;            // final mid, else last trade price, else 0
  double marked_pnl = 0;            // cash + position x mark_price
  // Sum over fills of qty x (fill price - mid when the order was sent), signed so that a
  // positive value is a cost. Only fills whose order saw both sides quoted at send count.
  double slippage = 0;
  std::uint64_t slippage_qty = 0;

  double fill_rate() const noexcept {
    return submitted_qty ? static_cast<double>(filled_qty) / static_cast<double>(submitted_qty) : 0;
  }
  double avg_slippage() const noexcept {
    return slippage_qty ? slippage / static_cast<double>(slippage_qty) : 0;
  }
};

struct TradingStats {
  ReplayStats replay;  // same meaning as replay()'s stats; trades/volume include strategy fills
  StrategyMetrics strategy;
};

TradingStats replay_trading(std::span<const Event> events, const TradingConfig& cfg,
                            ITradingStrategy& strategy);

// Handed to the strategy. Actions are queued, never applied synchronously: each one reaches the
// book `order_latency_ns` after now().
class OrderGateway {
 public:
  lob::OrderId submit(lob::Side side, lob::OrderType type, lob::Price price, lob::Qty qty);
  void cancel(lob::OrderId id);
  void modify(lob::OrderId id, lob::Price price, lob::Qty qty);  // same rules as OrderBook::modify
  lob::Timestamp now() const noexcept { return now_; }

 private:
  friend TradingStats replay_trading(std::span<const Event>, const TradingConfig&,
                                     ITradingStrategy&);
  enum class Kind : std::uint8_t { Submit, Cancel, Modify };
  struct Action {
    Kind kind;
    lob::Timestamp at;  // arrival time at the book
    lob::OrderId id;
    lob::Side side;
    lob::OrderType type;
    lob::Price price;
    lob::Qty qty;
    std::int64_t mid2;  // 2 x mid at send time (exact in ticks); valid if has_mid
    bool has_mid;
  };
  OrderGateway(const lob::OrderBook& book, lob::Timestamp latency) : book_(book), latency_(latency) {}
  void push(Action a);

  const lob::OrderBook& book_;
  lob::Timestamp latency_;
  lob::Timestamp now_ = 0;
  lob::OrderId next_id_ = kStrategyIdBase;
  std::deque<Action> pending_;  // arrival times never decrease, so FIFO order is arrival order
};

class ITradingStrategy {
 public:
  virtual ~ITradingStrategy() = default;
  // After each feed event has been applied (and its fills delivered).
  virtual void on_event(const Event&, const lob::OrderBook&, OrderGateway&) {}
  // After the book operation that produced the fill has finished, so the book is consistent.
  virtual void on_fill(const Fill&, OrderGateway&) {}
};

}  // namespace replay
