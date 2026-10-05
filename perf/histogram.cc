#include "perf/histogram.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace {

// With 16 ranges per doubling, a value's range is decided by its leading one
// bit and the four bits after it. Values below 16 have a range each.
constexpr size_t kRangesPerDoubling = 16;
constexpr int kBitsPerDoubling = 4;

// The index of the range that `nanoseconds` falls in.
size_t RangeOf(uint64_t nanoseconds) {
  if (nanoseconds < kRangesPerDoubling) return nanoseconds;
  // Position of the leading one bit, counting from zero.
  const int exponent = std::bit_width(nanoseconds) - 1;
  const uint64_t within_doubling =
      (nanoseconds >> (exponent - kBitsPerDoubling)) & (kRangesPerDoubling - 1);
  return static_cast<size_t>(exponent - kBitsPerDoubling + 1) *
             kRangesPerDoubling +
         within_doubling;
}

// The largest value that falls in range `index`; the inverse of RangeOf.
uint64_t UpperBoundOf(size_t index) {
  if (index < kRangesPerDoubling) return index;
  const size_t shift = index / kRangesPerDoubling - 1;
  const uint64_t lowest = (kRangesPerDoubling + index % kRangesPerDoubling)
                          << shift;
  return lowest + (uint64_t{1} << shift) - 1;
}

}  // namespace

void LatencyHistogram::Record(std::chrono::nanoseconds latency) {
  latency = std::max(latency, std::chrono::nanoseconds::zero());
  ++counts_[RangeOf(static_cast<uint64_t>(latency.count()))];
  ++count_;
  max_ = std::max(max_, latency);
  total_ += latency;
}

void LatencyHistogram::Merge(const LatencyHistogram& other) {
  for (size_t i = 0; i < counts_.size(); ++i) counts_[i] += other.counts_[i];
  count_ += other.count_;
  max_ = std::max(max_, other.max_);
  total_ += other.total_;
}

std::chrono::nanoseconds LatencyHistogram::Percentile(double percent) const {
  if (count_ == 0) return std::chrono::nanoseconds::zero();
  // How many measurements must be at or below the answer.
  const uint64_t needed = std::max<uint64_t>(
      1, static_cast<uint64_t>(
             std::ceil(percent / 100 * static_cast<double>(count_))));
  uint64_t seen = 0;
  for (size_t i = 0; i < counts_.size(); ++i) {
    seen += counts_[i];
    if (seen >= needed) {
      // The range's upper bound can overshoot the largest value recorded.
      return std::min(std::chrono::nanoseconds(UpperBoundOf(i)), max_);
    }
  }
  return max_;
}

std::chrono::nanoseconds LatencyHistogram::Mean() const {
  if (count_ == 0) return std::chrono::nanoseconds::zero();
  return total_ / count_;
}
