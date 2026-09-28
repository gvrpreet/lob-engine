#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <iterator>
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

  // The next level in priority order strictly after `price` (whether or not `price` is stored).
  std::optional<Price> next_after(Price price) const {
    const auto it = levels_.upper_bound(price);
    return it == levels_.end() ? std::nullopt : std::optional<Price>(it->first);
  }

  void erase(Price price) { levels_.erase(price); }

  template <class Function>
  void for_each_sorted(Function&& function) const {
    for (const auto& [price, level] : levels_) function(price, level);
  }

 private:
  std::map<Price, PriceLevel, Compare> levels_;
};

// Dense band of `span` consecutive prices with an occupancy bitmap; prices outside the band
// remain valid through the ordered overflow map. By default the band follows the market: it is
// centred on the first price stored, and re-centred on the new best price whenever the touch
// leaves it (a new level outside the band that becomes this side's best). The original fixed
// 9,000-11,000 band only fitted the synthetic generator, and every real ITCH price overflowed.
// Passing `base` pins the band (no re-centring).
template <class Compare>
class DensePriceLevels {
 public:
  explicit DensePriceLevels(std::size_t span = 2'001)
      : follow_(span != 0), levels_(span), occupied_((span + 63) / 64) {
    check_span(span);
  }
  DensePriceLevels(Price base, std::size_t span)
      : base_(base), anchored_(true), levels_(span), occupied_((span + 63) / 64) {
    check_span(span);
    if (span != 0 && base_ > std::numeric_limits<Price>::max() - static_cast<Price>(span - 1)) {
      throw std::length_error("dense price range exceeds Price bounds");
    }
  }

  bool empty() const noexcept { return size_ == 0; }
  std::size_t size() const noexcept { return size_; }
  std::size_t recentres() const noexcept { return recentres_; }  // band rebuilds so far

  PriceLevel& get_or_create(Price price) {
    if (!anchored_) anchor(price);
    if (const auto index = dense_index(price)) {
      const std::size_t word = *index / 64;
      const std::uint64_t mask = std::uint64_t{1} << (*index % 64);
      if ((occupied_[word] & mask) == 0) {
        occupied_[word] |= mask;
        ++size_;
      }
      return levels_[*index];
    }
    return get_or_create_outside(price);
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

  // The next level in priority order strictly after `price` (whether or not `price` is stored):
  // the nearer of the next occupied band slot and the next overflow level.
  std::optional<Price> next_after(Price price) const {
    std::optional<Price> dense;
    if (!levels_.empty()) {
      const Price last = price_at(levels_.size() - 1);
      if constexpr (kDescending) {  // bids: worse = lower price = lower index
        if (price > base_) {
          const std::size_t end = price > last ? levels_.size() : static_cast<std::size_t>(price - base_);
          if (const std::size_t i = prev_set_below(end); i != kNoIndex) dense = price_at(i);
        }
      } else {  // asks: worse = higher price = higher index
        if (price < last) {
          const std::size_t start = price < base_ ? 0 : static_cast<std::size_t>(price - base_) + 1;
          if (const std::size_t i = next_set_from(start); i != kNoIndex) dense = price_at(i);
        }
      }
    }
    const auto it = overflow_.upper_bound(price);
    const std::optional<Price> over =
        it == overflow_.end() ? std::nullopt : std::optional<Price>(it->first);
    if (!dense) return over;
    if (!over) return dense;
    return Compare{}(*dense, *over) ? dense : over;
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

  static void check_span(std::size_t span) {
    if (span > static_cast<std::size_t>(std::numeric_limits<Price>::max()))
      throw std::length_error("dense price range exceeds Price bounds");
  }

  // Centre the band on `price`, clamped so the whole band stays inside Price's range.
  void anchor(Price price) noexcept {
    const auto half = static_cast<Price>(levels_.size() / 2);
    const Price lowest = std::numeric_limits<Price>::min();
    const Price highest_base =
        levels_.empty() ? std::numeric_limits<Price>::max()
                        : std::numeric_limits<Price>::max() - static_cast<Price>(levels_.size() - 1);
    base_ = price < lowest + half ? lowest : std::min(price - half, highest_base);
    anchored_ = true;
  }

  // Out of line on purpose: prices outside the band are rare (4-15% of new levels on ITCH), and
  // inlining this path (map insert, best-price scan, rebuild) into get_or_create pushed the flat
  // index and get_or_create itself out of the replay loop once dense became the default
  // (+37% instructions/event on the v1 1M log).
  [[gnu::noinline]] PriceLevel& get_or_create_outside(Price price) {
    if (follow_ && becomes_best(price)) {
      // The touch has left the band: rebuild it around the new best price. Callers only create
      // levels outside matching, so no PriceLevel pointer is held across the move.
      recentre(price);
      return get_or_create(price);  // now inside the band (anchor keeps it in range)
    }
    auto [it, inserted] = overflow_.try_emplace(price);
    if (inserted) ++size_;
    return it->second;
  }

  // Would a new level at `price` be strictly better than every level on this side?
  bool becomes_best(Price price) const noexcept {
    const auto best = best_price();
    return !best || Compare{}(price, *best);
  }

  // Move every level (dense and overflow) into a band centred on `centre`. O(levels); runs only
  // when the touch leaves the band, i.e. after the market has moved by about span / 2 ticks.
  [[gnu::noinline, gnu::cold]] void recentre(Price centre) {
    std::vector<std::pair<Price, PriceLevel>> moved;
    moved.reserve(size_);
    for (std::size_t word = 0; word < occupied_.size(); ++word) {
      for (std::uint64_t bits = occupied_[word]; bits != 0; bits &= bits - 1) {
        const std::size_t index = word * 64 + static_cast<std::size_t>(std::countr_zero(bits));
        moved.emplace_back(price_at(index), levels_[index]);
        levels_[index] = PriceLevel{};
      }
      occupied_[word] = 0;
    }
    for (const auto& [price, level] : overflow_) moved.emplace_back(price, level);
    overflow_.clear();
    anchor(centre);
    ++recentres_;
    for (const auto& [price, level] : moved) {
      if (const auto index = dense_index(price)) {
        occupied_[*index / 64] |= std::uint64_t{1} << (*index % 64);
        levels_[*index] = level;
      } else {
        overflow_.emplace(price, level);
      }
    }
  }

  std::optional<std::size_t> dense_index(Price price) const noexcept {
    if (price < base_) return std::nullopt;  // before anchoring the bitmap is empty: all misses
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

  // First occupied index >= start, or kNoIndex.
  std::size_t next_set_from(std::size_t start) const noexcept {
    if (start >= levels_.size()) return kNoIndex;
    std::size_t word = start / 64;
    std::uint64_t bits = occupied_[word] & (~std::uint64_t{0} << (start % 64));
    while (bits == 0) {
      if (++word == occupied_.size()) return kNoIndex;
      bits = occupied_[word];
    }
    return word * 64 + static_cast<std::size_t>(std::countr_zero(bits));
  }

  // Last occupied index < end, or kNoIndex.
  std::size_t prev_set_below(std::size_t end) const noexcept {
    if (end == 0) return kNoIndex;
    const std::size_t last = end - 1;
    std::size_t word = last / 64;
    const unsigned keep = static_cast<unsigned>(last % 64) + 1;  // bits 0..last%64
    std::uint64_t bits = occupied_[word] & (keep == 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << keep) - 1);
    while (bits == 0) {
      if (word == 0) return kNoIndex;
      bits = occupied_[--word];
    }
    return word * 64 + (63 - static_cast<std::size_t>(std::countl_zero(bits)));
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

  Price base_ = 0;
  bool anchored_ = false;
  bool follow_ = false;  // re-centre when the touch leaves the band (default-constructed only)
  std::size_t recentres_ = 0;
  std::vector<PriceLevel> levels_;
  std::vector<std::uint64_t> occupied_;
  std::map<Price, PriceLevel, Compare> overflow_;
  std::size_t size_ = 0;
};

template <class Compare>
class SortedPriceLevels {
 public:
  bool empty() const noexcept { return levels_.empty(); }
  std::size_t size() const noexcept { return levels_.size(); }

  PriceLevel& get_or_create(Price price) {
    auto it = lower_bound(price);
    if (it != levels_.end() && it->price == price) return it->level;
    it = levels_.insert(it, Entry{price, PriceLevel{}});
    return it->level;
  }

  PriceLevel* find(Price price) noexcept {
    const auto it = lower_bound(price);
    return it == levels_.end() || it->price != price ? nullptr : &it->level;
  }

  const PriceLevel* find(Price price) const noexcept {
    const auto it = lower_bound(price);
    return it == levels_.end() || it->price != price ? nullptr : &it->level;
  }

  std::optional<Price> best_price() const noexcept {
    return levels_.empty() ? std::nullopt : std::optional<Price>(levels_.back().price);
  }

  PriceLevel* best(Price& price) noexcept {
    if (levels_.empty()) return nullptr;
    price = levels_.back().price;
    return &levels_.back().level;
  }

  // The next level in priority order strictly after `price`. Entries run worst → best, and
  // lower_bound finds the first entry at or better than `price`: the next worse is just before.
  std::optional<Price> next_after(Price price) const {
    const auto it = lower_bound(price);
    if (it == levels_.begin()) return std::nullopt;
    return std::prev(it)->price;
  }

  void erase(Price price) {
    const auto it = lower_bound(price);
    if (it != levels_.end() && it->price == price) levels_.erase(it);
  }

  template <class Function>
  void for_each_sorted(Function&& function) const {
    for (auto it = levels_.rbegin(); it != levels_.rend(); ++it) {
      function(it->price, it->level);
    }
  }

 private:
  struct Entry {
    Price price;
    PriceLevel level;
  };

  using Iterator = typename std::vector<Entry>::iterator;
  using ConstIterator = typename std::vector<Entry>::const_iterator;

  Iterator lower_bound(Price price) noexcept {
    return std::lower_bound(levels_.begin(), levels_.end(), price,
                            [](const Entry& entry, Price value) {
                              return Compare{}(value, entry.price);
                            });
  }

  ConstIterator lower_bound(Price price) const noexcept {
    return std::lower_bound(levels_.begin(), levels_.end(), price,
                            [](const Entry& entry, Price value) {
                              return Compare{}(value, entry.price);
                            });
  }

  std::vector<Entry> levels_;
};

}  // namespace lob
