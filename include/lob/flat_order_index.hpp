#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

#include "lob/page_allocator.hpp"
#include "lob/types.hpp"

namespace lob {

// Alloc = HugePageAllocator backs the table with THP (phase 2d storage variant).
template <template <class> class Alloc = std::allocator>
class BasicFlatOrderIndex {
 public:
  explicit BasicFlatOrderIndex(std::size_t max_entries)
      : slots_(table_size_for(max_entries)),
        shift_(64 - static_cast<unsigned>(std::countr_zero(slots_.size()))) {}

  std::size_t size() const noexcept { return size_; }

  bool contains(OrderId id) const noexcept { return find_slot(id) != kNotFound; }

  bool find(OrderId id, Handle& handle) const noexcept {
    const std::size_t slot = find_slot(id);
    if (slot == kNotFound) return false;
    handle = slots_[slot].handle;
    return true;
  }

  bool insert(OrderId id, Handle handle) noexcept {
    const std::size_t mask = slots_.size() - 1;
    std::size_t slot = home(id);
    for (std::size_t probes = 0; probes < slots_.size(); ++probes) {
      Entry& entry = slots_[slot];
      if (entry.handle == kInvalid) {
        entry.id = id;
        entry.handle = handle;
        ++size_;
        return true;
      }
      if (entry.id == id) return false;
      slot = (slot + 1) & mask;
    }
    return false;
  }

  bool erase(OrderId id) noexcept {
    std::size_t hole = find_slot(id);
    if (hole == kNotFound) return false;

    const std::size_t mask = slots_.size() - 1;
    std::size_t current = (hole + 1) & mask;
    while (slots_[current].handle != kInvalid) {
      const std::size_t wanted = home(slots_[current].id);
      if (((current - wanted) & mask) >= ((current - hole) & mask)) {
        slots_[hole] = slots_[current];
        hole = current;
      }
      current = (current + 1) & mask;
    }
    slots_[hole].handle = kInvalid;
    --size_;
    return true;
  }

 private:
  struct Entry {
    OrderId id = 0;
    Handle handle = kInvalid;
  };

  static constexpr std::size_t kNotFound = std::numeric_limits<std::size_t>::max();

  static std::size_t table_size_for(std::size_t max_entries) {
    std::size_t table_size = 2;
    while (table_size <= max_entries) {
      if (table_size > std::numeric_limits<std::size_t>::max() / 2) {
        throw std::length_error("order index capacity is too large");
      }
      table_size *= 2;
    }
    return table_size;
  }

  // Home slot. Blocks of 2^6 = 64 consecutive ids (1 KB of entries) stay together, and the
  // blocks are scattered by Fibonacci hashing (multiply by 2^64/phi, keep the top bits).
  // It was std::hash<OrderId>, which libstdc++ implements as the identity. Sequential ids plus
  // linear probing then formed long primary clusters once ids had wrapped the table a few times
  // with old orders still resting, and replay collapsed from 15.5M ev/s at 10M events to 0.41M
  // at 15M. Scattering every id fixes that but loses the identity hash's locality for recent
  // ids (-43% at 1M). 64-id blocks keep most of it and are fastest from 10M to 50M events.
  // See docs/learning/experiments/2026-09-28-index-hash-clustering.md.
#if !defined(LOB_INDEX_BLOCK_BITS)
#define LOB_INDEX_BLOCK_BITS 6  // experiment knob: 2^bits consecutive ids stay together
#endif
  std::size_t home(OrderId id) const noexcept {
    constexpr unsigned kBits = LOB_INDEX_BLOCK_BITS;
    if constexpr (kBits > 0) {
      if (shift_ + kBits < 64) {  // the table has more than one block
        const std::size_t block = static_cast<std::size_t>(
            ((id >> kBits) * 0x9E3779B97F4A7C15ULL) >> (shift_ + kBits));
        return (block << kBits) | static_cast<std::size_t>(id & ((OrderId{1} << kBits) - 1));
      }
    }
    return static_cast<std::size_t>((id * 0x9E3779B97F4A7C15ULL) >> shift_);
  }

  std::size_t find_slot(OrderId id) const noexcept {
    const std::size_t mask = slots_.size() - 1;
    std::size_t slot = home(id);
    for (std::size_t probes = 0; probes < slots_.size(); ++probes) {
      const Entry& entry = slots_[slot];
      if (entry.handle == kInvalid) return kNotFound;
      if (entry.id == id) return slot;
      slot = (slot + 1) & mask;
    }
    return kNotFound;
  }

  std::vector<Entry, Alloc<Entry>> slots_;
  unsigned shift_;  // 64 - log2(table size); the table size is a power of two >= 2
  std::size_t size_ = 0;
};

using FlatOrderIndex = BasicFlatOrderIndex<>;

}  // namespace lob