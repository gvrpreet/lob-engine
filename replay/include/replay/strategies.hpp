#pragma once
#include "replay/replayer.hpp"

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

}  // namespace replay
