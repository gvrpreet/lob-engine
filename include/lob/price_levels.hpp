#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include "lob/price_level.hpp"
#include "lob/types.hpp"

namespace lob {

template <class Compare>
class TreePriceLevels {
 public:
  bool empty() const noexcept { return levels_.empty(); }
  std::size_t size() const noexcept { return levels_.size(); }

  PriceLevel& get_or_create(Price price) { return levels_[price]; }

  PriceLevel* find(Price price) noexcept {
    const auto it = levels_.find(price);
    return it == levels_.end() ? nullptr : &it->second;
  }

  const PriceLevel* find(Price price) const noexcept {
    const auto it = levels_.find(price);
    return it == levels_.end() ? nullptr : &it->second;
  }

  std::optional<Price> best_price() const noexcept {
    return levels_.empty() ? std::nullopt : std::optional<Price>(levels_.begin()->first);
  }

  PriceLevel* best(Price& price) noexcept {
    if (levels_.empty()) return nullptr;
    price = levels_.begin()->first;
    return &levels_.begin()->second;
  }

  void erase(Price price) { levels_.erase(price); }

  template <class Function>
  void for_each_sorted(Function&& function) const {
    for (const auto& [price, level] : levels_) function(price, level);
  }

 private:
  std::map<Price, PriceLevel, Compare> levels_;
};

// Dense central band for the synthetic-market experiment; arbitrary prices
// remain valid through the ordered overflow map.
template <class Compare>
class DensePriceLevels {
 public:
  explicit DensePriceLevels(Price base = 9'000, std::size_t span = 2'001)
      : base_(base), levels_(span), occupied_((span + 63) / 64) {
    if (span > static_cast<std::size_t>(std::numeric_limits<Price>::max()) ||
        (span != 0 && base_ > std::numeric_limits<Price>::max() -
                                   static_cast<Price>(span - 1))) {
      throw std::length_error("dense price range exceeds Price bounds");
    }
  }

  bool empty() const noexcept { return size_ == 0; }
  std::size_t size() const noexcept { return size_; }

  PriceLevel& get_or_create(Price price) {
    if (const auto index = dense_index(price)) {
      const std::size_t word = *index / 64;
      const std::uint64_t mask = std::uint64_t{1} << (*index % 64);
      if ((occupied_[word] & mask) == 0) {
        occupied_[word] |= mask;
        ++size_;
      }
      return levels_[*index];
    }

    auto [it, inserted] = overflow_.try_emplace(price);
    if (inserted) ++size_;
    return it->second;
  }

  PriceLevel* find(Price price) noexcept {
    if (const auto index = dense_index(price)) {
      return is_occupied(*index) ? &levels_[*index] : nullptr;
    }
    const auto it = overflow_.find(price);
    return it == overflow_.end() ? nullptr : &it->second;
  }

  const PriceLevel* find(Price price) const noexcept {
    if (const auto index = dense_index(price)) {
      return is_occupied(*index) ? &levels_[*index] : nullptr;
    }
    const auto it = overflow_.find(price);
    return it == overflow_.end() ? nullptr : &it->second;
  }

  std::optional<Price> best_price() const noexcept {
    const std::size_t index = dense_best_index();
    const bool has_dense = index != kNoIndex;
    const bool has_overflow = !overflow_.empty();
    if (!has_dense) {
      return has_overflow ? std::optional<Price>(overflow_.begin()->first) : std::nullopt;
    }
    const Price dense_price = price_at(index);
    if (!has_overflow || Compare{}(dense_price, overflow_.begin()->first)) return dense_price;
    return overflow_.begin()->first;
  }

  PriceLevel* best(Price& price) noexcept {
    const auto best = best_price();
    if (!best) return nullptr;
    price = *best;
    return find(price);
  }

  void erase(Price price) {
    if (const auto index = dense_index(price)) {
      const std::size_t word = *index / 64;
      const std::uint64_t mask = std::uint64_t{1} << (*index % 64);
      if ((occupied_[word] & mask) != 0) {
        levels_[*index] = PriceLevel{};
        occupied_[word] &= ~mask;
        --size_;
      }
      return;
    }
    if (overflow_.erase(price) != 0) --size_;
  }

  template <class Function>
  void for_each_sorted(Function&& function) const {
    constexpr bool descending = std::is_same_v<Compare, std::greater<Price>>;
    std::size_t cursor = descending ? levels_.size() : 0;
    std::size_t index = 0;
    bool has_dense = next_dense(cursor, index);
    auto overflow = overflow_.begin();

    while (has_dense || overflow != overflow_.end()) {
      const Price dense_price = has_dense ? price_at(index) : Price{};
      const bool take_dense = has_dense &&
                              (overflow == overflow_.end() ||
                               Compare{}(dense_price, overflow->first));
      if (take_dense) {
        function(dense_price, levels_[index]);
        has_dense = next_dense(cursor, index);
      } else {
        function(overflow->first, overflow->second);
        ++overflow;
      }
    }
  }

 private:
  static constexpr std::size_t kNoIndex = std::numeric_limits<std::size_t>::max();
  static constexpr bool kDescending = std::is_same_v<Compare, std::greater<Price>>;

  std::optional<std::size_t> dense_index(Price price) const noexcept {
    if (price < base_) return std::nullopt;
    const std::uint64_t offset = static_cast<std::uint64_t>(price) -
                                 static_cast<std::uint64_t>(base_);
    if (offset >= levels_.size()) return std::nullopt;
    return static_cast<std::size_t>(offset);
  }

  bool is_occupied(std::size_t index) const noexcept {
    return (occupied_[index / 64] & (std::uint64_t{1} << (index % 64))) != 0;
  }

  std::size_t dense_best_index() const noexcept {
    if constexpr (kDescending) {
      for (std::size_t word = occupied_.size(); word > 0; --word) {
        const std::uint64_t bits = occupied_[word - 1];
        if (bits != 0) {
          return (word - 1) * 64 + (63 - static_cast<std::size_t>(std::countl_zero(bits)));
        }
      }
    } else {
      for (std::size_t word = 0; word < occupied_.size(); ++word) {
        if (occupied_[word] != 0) {
          return word * 64 + static_cast<std::size_t>(std::countr_zero(occupied_[word]));
        }
      }
    }
    return kNoIndex;
  }

  bool next_dense(std::size_t& cursor, std::size_t& index) const noexcept {
    if constexpr (kDescending) {
      while (cursor > 0) {
        --cursor;
        if (is_occupied(cursor)) {
          index = cursor;
          return true;
        }
      }
    } else {
      while (cursor < levels_.size()) {
        if (is_occupied(cursor)) {
          index = cursor++;
          return true;
        }
        ++cursor;
      }
    }
    return false;
  }

  Price price_at(std::size_t index) const noexcept {
    return base_ + static_cast<Price>(index);
  }

  Price base_;
  std::vector<PriceLevel> levels_;
  std::vector<std::uint64_t> occupied_;
  std::map<Price, PriceLevel, Compare> overflow_;
  std::size_t size_ = 0;
};

}  // namespace lob
