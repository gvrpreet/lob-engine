#pragma once
#include <cstdint>

#include "lob/order_pool.hpp"

namespace lob {

// FIFO queue of orders at one price. Time priority == list order.
struct PriceLevel {
  Handle head = kInvalid;
  Handle tail = kInvalid;
  Qty total_qty = 0;
  std::uint32_t count = 0;

  bool empty() const noexcept { return head == kInvalid; }

  template <class Pool>
  void push_back(Pool& pool, Handle h) noexcept {
    pool.set_prev(h, tail);
    pool.set_next(h, kInvalid);
    if (tail != kInvalid) pool.set_next(tail, h); else head = h;
    tail = h;
    total_qty += pool.qty(h);
    ++count;
  }
  // if FIFO is replaced by LIFO, 56406 fails because breaking time priority 
  // produces different fill sequences than the naive oracle on almost every random operation. 
  // This is what the differential harness is for: it doesn't need to know in advance what 
  // "FIFO" means, it just needs the reference implementation to disagree loudly.


  // O(1) removal from anywhere in the queue (this is what makes cancel cheap).
  template <class Pool>
  void unlink(Pool& pool, Handle h) noexcept {
    const Handle previous = pool.prev(h);
    const Handle next_order = pool.next(h);
    if (previous != kInvalid) pool.set_next(previous, next_order); else head = next_order;
    if (next_order != kInvalid) pool.set_prev(next_order, previous); else tail = previous;
    total_qty -= pool.qty(h);
    --count;
  }
};

}  // namespace lob
