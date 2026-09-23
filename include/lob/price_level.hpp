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
