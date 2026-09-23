#pragma once
#include <cstddef>
#include <vector>

#include "lob/order.hpp"

namespace lob {

// Fixed-capacity slab of Orders with an O(1) free list.
// - No allocation on the hot path (all memory reserved up front).
// - Slots never move, so Handles stay valid for the order's lifetime.
class OrderPool {
 public:
  explicit OrderPool(std::size_t capacity) : slots_(capacity) {
    for (std::size_t i = 0; i < capacity; ++i) {
      slots_[i].next = (i + 1 < capacity) ? static_cast<Handle>(i + 1) : kInvalid;
    }
    free_head_ = capacity ? 0 : kInvalid;
  }

  Handle allocate() noexcept {
    if (free_head_ == kInvalid) return kInvalid;
    const Handle h = free_head_;
    free_head_ = slots_[h].next;
    ++live_;
    return h;
  }

  void release(Handle h) noexcept {
    slots_[h].next = free_head_;
    free_head_ = h;
    --live_;
  }

  Order& operator[](Handle h) noexcept { return slots_[h]; }
  const Order& operator[](Handle h) const noexcept { return slots_[h]; }

  bool full() const noexcept { return free_head_ == kInvalid; }
  std::size_t live() const noexcept { return live_; }
  std::size_t capacity() const noexcept { return slots_.size(); }

 private:
  std::vector<Order> slots_;
  Handle free_head_ = kInvalid;
  std::size_t live_ = 0;
};

}  // namespace lob
