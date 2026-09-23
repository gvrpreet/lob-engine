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

  void push_back(OrderPool& pool, Handle h) noexcept {
    Order& o = pool[h];
    o.prev = tail;
    o.next = kInvalid;
    if (tail != kInvalid) pool[tail].next = h; else head = h;
    tail = h;
    total_qty += o.qty;
    ++count;
  }
  // if FIFO is replaced by LIFO, 56406 fails because breaking time priority 
  // produces different fill sequences than the naive oracle on almost every random operation. 
  // This is what the differential harness is for: it doesn't need to know in advance what 
  // "FIFO" means, it just needs the reference implementation to disagree loudly.


  // O(1) removal from anywhere in the queue (this is what makes cancel cheap).
  void unlink(OrderPool& pool, Handle h) noexcept {
    Order& o = pool[h];
    if (o.prev != kInvalid) pool[o.prev].next = o.next; else head = o.next;
    if (o.next != kInvalid) pool[o.next].prev = o.prev; else tail = o.prev;
    total_qty -= o.qty;
    --count;
  }
};

}  // namespace lob
