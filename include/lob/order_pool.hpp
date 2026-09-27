#pragma once
#include <cstddef>
#include <memory>
#include <vector>

#if defined(__linux__)
#include <sys/mman.h>
#endif

#include "lob/order.hpp"
#include "lob/page_allocator.hpp"

namespace lob {

// Fixed-capacity slab of Orders with an O(1) free list.
// - No allocation on the hot path (all memory reserved up front).
// - Slots never move, so Handles stay valid for the order's lifetime.
// - The constructor writes every slot (free list), so all pages are faulted in before use.
//   Alloc = HugePageAllocator requests THP for the slab before that first touch.
template <class StoredOrder, template <class> class Alloc = std::allocator>
class BasicOrderPool {
 public:
  using OrderType = StoredOrder;

  explicit BasicOrderPool(std::size_t capacity) : slots_(capacity) {
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

  StoredOrder& operator[](Handle h) noexcept { return slots_[h]; }
  const StoredOrder& operator[](Handle h) const noexcept { return slots_[h]; }

  void set_order(Handle h, OrderId id, Price price, Qty qty, Side side) noexcept {
    slots_[h].id = id;
    slots_[h].price = price;
    slots_[h].qty = qty;
    slots_[h].side = side;
  }
  OrderId id(Handle h) const noexcept { return slots_[h].id; }
  Price price(Handle h) const noexcept { return slots_[h].price; }
  Qty qty(Handle h) const noexcept { return slots_[h].qty; }
  Side side(Handle h) const noexcept { return slots_[h].side; }
  Handle prev(Handle h) const noexcept { return slots_[h].prev; }
  Handle next(Handle h) const noexcept { return slots_[h].next; }
  void set_qty(Handle h, Qty qty) noexcept { slots_[h].qty = qty; }
  void set_prev(Handle h, Handle prev) noexcept { slots_[h].prev = prev; }
  void set_next(Handle h, Handle next) noexcept { slots_[h].next = next; }

  bool full() const noexcept { return free_head_ == kInvalid; }
  std::size_t live() const noexcept { return live_; }
  std::size_t capacity() const noexcept { return slots_.size(); }

  // Largely redundant: construction already touched every slot. Kept for the phase 2(d) test.
  void prefault() noexcept {
    if (slots_.empty()) return;
    volatile StoredOrder* p = const_cast<volatile StoredOrder*>(slots_.data());
    for (std::size_t i = 0; i < slots_.size(); ++i) {
      (void)p[i];
    }
#if defined(__linux__)
    std::size_t bytes = slots_.size() * sizeof(StoredOrder);
    (void)madvise(const_cast<void*>(static_cast<const void*>(slots_.data())), bytes, MADV_WILLNEED);
#endif
  }

 private:
  std::vector<StoredOrder, Alloc<StoredOrder>> slots_;
  Handle free_head_ = kInvalid;
  std::size_t live_ = 0;
};

using OrderPool = BasicOrderPool<Order>;
using HugePageOrderPool = BasicOrderPool<Order, HugePageAllocator>;
using OrderPool48 = BasicOrderPool<Order48>;
using OrderPool64 = BasicOrderPool<Order64>;

// Benchmark baseline: preserve the same handle/free-list interface, but allocate
// and free each live Order separately instead of storing Orders in a slab.
template <class StoredOrder>
class BasicHeapOrderPool {
 public:
  using OrderType = StoredOrder;

  explicit BasicHeapOrderPool(std::size_t capacity) : slots_(capacity) {
    for (std::size_t i = 0; i < capacity; ++i) {
      slots_[i].next = (i + 1 < capacity) ? static_cast<Handle>(i + 1) : kInvalid;
    }
    free_head_ = capacity ? 0 : kInvalid;
  }

  Handle allocate() {
    if (free_head_ == kInvalid) return kInvalid;
    OrderPtr order = allocate_order();
    const Handle handle = free_head_;
    free_head_ = slots_[handle].next;
    slots_[handle].order = std::move(order);
    ++live_;
    return handle;
  }

  void release(Handle handle) noexcept {
    slots_[handle].order.reset();
    slots_[handle].next = free_head_;
    free_head_ = handle;
    --live_;
  }

  StoredOrder& operator[](Handle handle) noexcept { return *slots_[handle].order; }
  const StoredOrder& operator[](Handle handle) const noexcept { return *slots_[handle].order; }

  void set_order(Handle handle, OrderId id, Price price, Qty qty, Side side) noexcept {
    StoredOrder& order = *slots_[handle].order;
    order.id = id;
    order.price = price;
    order.qty = qty;
    order.side = side;
  }
  OrderId id(Handle handle) const noexcept { return slots_[handle].order->id; }
  Price price(Handle handle) const noexcept { return slots_[handle].order->price; }
  Qty qty(Handle handle) const noexcept { return slots_[handle].order->qty; }
  Side side(Handle handle) const noexcept { return slots_[handle].order->side; }
  Handle prev(Handle handle) const noexcept { return slots_[handle].order->prev; }
  Handle next(Handle handle) const noexcept { return slots_[handle].order->next; }
  void set_qty(Handle handle, Qty qty) noexcept { slots_[handle].order->qty = qty; }
  void set_prev(Handle handle, Handle prev) noexcept { slots_[handle].order->prev = prev; }
  void set_next(Handle handle, Handle next) noexcept { slots_[handle].order->next = next; }

  bool full() const noexcept { return free_head_ == kInvalid; }
  std::size_t live() const noexcept { return live_; }
  std::size_t capacity() const noexcept { return slots_.size(); }

  void prefault() noexcept {
    for (std::size_t i = 0; i < slots_.size(); ++i) {
      if (slots_[i].order) {
        (void)*slots_[i].order;
      }
    }
#if defined(__linux__)
    if (!slots_.empty()) {
      std::size_t bytes = slots_.size() * sizeof(Slot);
      (void)madvise(static_cast<void*>(slots_.data()), bytes, MADV_WILLNEED);
    }
#endif
  }

 private:
  using Allocator = std::allocator<StoredOrder>;

  struct Deleter {
    void operator()(StoredOrder* order) const noexcept {
      Allocator allocator;
      std::allocator_traits<Allocator>::destroy(allocator, order);
      std::allocator_traits<Allocator>::deallocate(allocator, order, 1);
    }
  };

  using OrderPtr = std::unique_ptr<StoredOrder, Deleter>;

  static OrderPtr allocate_order() {
    Allocator allocator;
    StoredOrder* order = std::allocator_traits<Allocator>::allocate(allocator, 1);
    try {
      std::allocator_traits<Allocator>::construct(allocator, order);
    } catch (...) {
      std::allocator_traits<Allocator>::deallocate(allocator, order, 1);
      throw;
    }
    return OrderPtr(order);
  }

  struct Slot {
    OrderPtr order;
    Handle next = kInvalid;
  };

  std::vector<Slot> slots_;
  Handle free_head_ = kInvalid;
  std::size_t live_ = 0;
};

using HeapOrderPool = BasicHeapOrderPool<Order>;

class HotColdOrderPool {
 public:
  explicit HotColdOrderPool(std::size_t capacity) : hot_(capacity), cold_(capacity) {
    for (std::size_t i = 0; i < capacity; ++i) {
      hot_[i].next = (i + 1 < capacity) ? static_cast<Handle>(i + 1) : kInvalid;
    }
    free_head_ = capacity ? 0 : kInvalid;
  }

  Handle allocate() noexcept {
    if (free_head_ == kInvalid) return kInvalid;
    const Handle handle = free_head_;
    free_head_ = hot_[handle].next;
    ++live_;
    return handle;
  }

  void release(Handle handle) noexcept {
    hot_[handle].next = free_head_;
    free_head_ = handle;
    --live_;
  }

  void set_order(Handle handle, OrderId id, Price price, Qty qty, Side side) noexcept {
    cold_[handle] = ColdFields{id, price, side};
    hot_[handle].qty = qty;
  }
  OrderId id(Handle handle) const noexcept { return cold_[handle].id; }
  Price price(Handle handle) const noexcept { return cold_[handle].price; }
  Qty qty(Handle handle) const noexcept { return hot_[handle].qty; }
  Side side(Handle handle) const noexcept { return cold_[handle].side; }
  Handle prev(Handle handle) const noexcept { return hot_[handle].prev; }
  Handle next(Handle handle) const noexcept { return hot_[handle].next; }
  void set_qty(Handle handle, Qty qty) noexcept { hot_[handle].qty = qty; }
  void set_prev(Handle handle, Handle prev) noexcept { hot_[handle].prev = prev; }
  void set_next(Handle handle, Handle next) noexcept { hot_[handle].next = next; }

  bool full() const noexcept { return free_head_ == kInvalid; }
  std::size_t live() const noexcept { return live_; }
  std::size_t capacity() const noexcept { return hot_.size(); }

  void prefault() noexcept {
    for (std::size_t i = 0; i < hot_.size(); ++i) {
      (void)hot_[i];
      (void)cold_[i];
    }
#if defined(__linux__)
    if (!hot_.empty()) {
      std::size_t hot_bytes = hot_.size() * sizeof(HotFields);
      std::size_t cold_bytes = cold_.size() * sizeof(ColdFields);
      (void)madvise(const_cast<void*>(static_cast<const void*>(hot_.data())), hot_bytes, MADV_WILLNEED);
      (void)madvise(const_cast<void*>(static_cast<const void*>(cold_.data())), cold_bytes, MADV_WILLNEED);
    }
#endif
  }

 private:
  struct HotFields {
    Qty qty = 0;
    Handle prev = kInvalid;
    Handle next = kInvalid;
  };
  static_assert(sizeof(HotFields) == 12);

  struct ColdFields {
    OrderId id = 0;
    Price price = 0;
    Side side = Side::Buy;
  };
  static_assert(sizeof(ColdFields) == 24);

  std::vector<HotFields> hot_;
  std::vector<ColdFields> cold_;
  Handle free_head_ = kInvalid;
  std::size_t live_ = 0;
};

}  // namespace lob
