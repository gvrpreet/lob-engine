#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>

namespace replay {

// HDR-style log-linear histogram over uint64 values (ns). Values below 64 get their own bucket;
// above that, each power of two is split into 32 sub-buckets, so a bucket's width is at most 1/32
// of its lower bound (~3% relative error). Fixed ~15 KB of counters: recording is a few shifts and
// one increment into cache-resident memory, instead of appending to a per-event vector.
class LatencyHistogram {
 public:
  static constexpr unsigned kSubBucketBits = 5;
  static constexpr std::uint64_t kSubBuckets = std::uint64_t{1} << kSubBucketBits;  // 32
  static constexpr std::size_t kBucketCount = (64 - kSubBucketBits - 1) * kSubBuckets + 2 * kSubBuckets;

  static constexpr std::size_t bucket_index(std::uint64_t v) {
    if (v < kSubBuckets) return static_cast<std::size_t>(v);
    const auto shift = static_cast<unsigned>(std::bit_width(v)) - kSubBucketBits - 1;
    return static_cast<std::size_t>(shift * kSubBuckets + (v >> shift));
  }
  static constexpr std::uint64_t bucket_lowest(std::size_t i) {
    if (i < 2 * kSubBuckets) return i;
    const auto shift = static_cast<unsigned>(i / kSubBuckets) - 1;
    return (kSubBuckets + i % kSubBuckets) << shift;
  }
  static constexpr std::uint64_t bucket_highest(std::size_t i) {
    if (i < 2 * kSubBuckets) return i;
    const auto shift = static_cast<unsigned>(i / kSubBuckets) - 1;
    return bucket_lowest(i) + ((std::uint64_t{1} << shift) - 1);
  }

  void record(std::uint64_t v) {
    ++counts_[bucket_index(v)];
    ++count_;
    max_ = std::max(max_, v);
  }

  std::uint64_t count() const { return count_; }
  std::uint64_t max() const { return max_; }

  // Same rank as the exact nth_element definition (index floor(p/100 * (n-1)) of the sorted
  // samples); reports the highest value equivalent to that sample's bucket, capped at max().
  std::uint64_t percentile(double p) const {
    if (count_ == 0) return 0;
    p = std::clamp(p, 0.0, 100.0);
    const auto rank = static_cast<std::uint64_t>(p / 100.0 * static_cast<double>(count_ - 1));
    std::uint64_t seen = 0;
    for (std::size_t i = 0; i < kBucketCount; ++i) {
      seen += counts_[i];
      if (seen > rank) return std::min(bucket_highest(i), max_);
    }
    return max_;
  }

 private:
  std::array<std::uint64_t, kBucketCount> counts_{};
  std::uint64_t count_ = 0;
  std::uint64_t max_ = 0;
};

}  // namespace replay
