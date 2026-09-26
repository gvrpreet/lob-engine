#pragma once

#include <cstddef>
#include <functional>
#include <limits>
#include <stdexcept>
#include <vector>

#include "lob/types.hpp"

namespace lob {

class FlatOrderIndex {
 public:
  explicit FlatOrderIndex(std::size_t max_entries) : slots_(table_size_for(max_entries)) {}

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
    std::size_t slot = hash(id) & mask;
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
      const std::size_t home = hash(slots_[current].id) & mask;
      if (((current - home) & mask) >= ((current - hole) & mask)) {
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

  static std::size_t hash(OrderId id) noexcept { return std::hash<OrderId>{}(id); }

  std::size_t find_slot(OrderId id) const noexcept {
    const std::size_t mask = slots_.size() - 1;
    std::size_t slot = hash(id) & mask;
    for (std::size_t probes = 0; probes < slots_.size(); ++probes) {
      const Entry& entry = slots_[slot];
      if (entry.handle == kInvalid) return kNotFound;
      if (entry.id == id) return slot;
      slot = (slot + 1) & mask;
    }
    return kNotFound;
  }

  std::vector<Entry> slots_;
  std::size_t size_ = 0;
};

}  // namespace lob