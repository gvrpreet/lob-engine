#pragma once
#include <cstdint>
#include <optional>

#include "replay/replayer.hpp"
#include "replay/trading.hpp"

namespace replay {

// Smallest useful strategy: running VWAP + last trade price.
class VwapTracker final : public IStrategy {
 public:
  void on_trade(const lob::Trade& t) override {
    notional_ += static_cast<double>(t.price) * t.qty;
    volume_ += t.qty;
    last_ = t.price;
  }
  double vwap() const { return volume_ ? notional_ / static_cast<double>(volume_) : 0.0; }
  lob::Price last() const { return last_; }

 private:
  double notional_ = 0;
  std::uint64_t volume_ = 0;
  lob::Price last_ = 0;
};

// Minimal market maker, used to exercise order entry and metrics (Phase 5f/g), not to make
// money. It keeps one limit order per side at the touch (joining the best bid and best ask held
// by *others*), `size` shares each, and re-prices it with a Modify when that touch moves (losing
// queue position). It stops quoting a side, cancelling its order, once the position reaches
// `max_position` in that direction. It is deterministic: its decisions depend only on the book
// and its own fills.
class MarketMaker final : public ITradingStrategy {
 public:
  struct Params {
    lob::Qty size = 10;
    std::int64_t max_position = 200;
    std::uint32_t requote_every = 1;  // re-check quotes every N feed events
    // Skip price levels where only this strategy's own order rests when reading the touch. With
    // false (the original behaviour) a quote left alone at a price keeps "joining" itself, stays
    // stale after the market moves away, and is picked off (see the queue-position note).
    bool exclude_own_orders = true;
  };
  MarketMaker() = default;
  explicit MarketMaker(Params p) : p_(p) {}

  void on_event(const Event&, const lob::OrderBook& book, OrderGateway& gw) override {
    if (++events_ % p_.requote_every != 0) return;
    quote(lob::Side::Buy, others_touch(lob::Side::Buy, book), gw);
    quote(lob::Side::Sell, others_touch(lob::Side::Sell, book), gw);
  }

  void on_fill(const Fill& f, OrderGateway&) override {
    const auto q = static_cast<std::int64_t>(f.qty);
    position_ += f.side == lob::Side::Buy ? q : -q;
    Slot& s = slot(f.side);
    if (s.id == f.id && f.remaining == 0) s = {};  // gone: quote afresh on the next event
  }

  std::int64_t position() const noexcept { return position_; }

 private:
  struct Slot {
    lob::OrderId id = 0;  // 0 = no order on this side
    lob::Price price = 0;
  };
  Slot& slot(lob::Side side) { return side == lob::Side::Buy ? bid_ : ask_; }

  // Best price on `side` held by someone else: skips the level where this strategy's resting
  // order is all there is.
  std::optional<lob::Price> others_touch(lob::Side side, const lob::OrderBook& book) const {
    std::optional<lob::Price> touch = side == lob::Side::Buy ? book.best_bid() : book.best_ask();
    if (!p_.exclude_own_orders || !touch) return touch;
    const Slot& s = side == lob::Side::Buy ? bid_ : ask_;
    lob::Side own_side;
    lob::Price own_price;
    lob::Qty own_qty;
    if (s.id == 0 || !book.find_order(s.id, own_side, own_price, own_qty)) return touch;  // in flight
    if (*touch == own_price && book.volume_at(side, own_price) <= own_qty)
      touch = book.next_price_after(side, own_price);
    return touch;
  }

  void quote(lob::Side side, std::optional<lob::Price> touch, OrderGateway& gw) {
    Slot& s = slot(side);
    const bool allowed =
        side == lob::Side::Buy ? position_ < p_.max_position : position_ > -p_.max_position;
    if (!allowed || !touch) {
      if (s.id != 0) gw.cancel(s.id);
      s = {};
      return;
    }
    if (s.id == 0) {
      s = {gw.submit(side, lob::OrderType::Limit, *touch, p_.size), *touch};
    } else if (s.price != *touch) {
      gw.modify(s.id, *touch, p_.size);
      s.price = *touch;
    }
  }

  Params p_{};
  std::uint64_t events_ = 0;
  std::int64_t position_ = 0;
  Slot bid_, ask_;
};

}  // namespace replay
