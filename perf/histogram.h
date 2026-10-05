// Collecting latencies and asking what was typical.
//
// A benchmark makes millions of measurements. Keeping every one would cost
// more than the thing being measured, so a histogram keeps counts: how many
// measurements fell in each of a fixed set of ranges. That is enough to answer
// "how long did the median request take?" or "the slowest 1%?" to within a
// few percent, in a few kilobytes, with no allocation per measurement.

#ifndef PERF_HISTOGRAM_H_
#define PERF_HISTOGRAM_H_

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>

class LatencyHistogram {
 public:
  // Adds one measurement. Negative latencies count as zero.
  void Record(std::chrono::nanoseconds latency);

  // Adds every measurement in `other`. Lets each thread record into its own
  // histogram, with no locking, and the results be combined at the end.
  void Merge(const LatencyHistogram& other);

  // How many measurements have been recorded.
  uint64_t count() const { return count_; }

  // The latency that `percent` percent of measurements were at or below:
  // Percentile(50) is the median, Percentile(99) the level only the slowest
  // 1% exceeded. `percent` is in (0, 100].
  //
  // Approximate: the result is at least the true value and at most about 6%
  // above it. Zero if nothing has been recorded.
  std::chrono::nanoseconds Percentile(double percent) const;

  // The largest measurement, exactly. Zero if nothing has been recorded.
  std::chrono::nanoseconds max() const { return max_; }

  // The average measurement, exactly. Zero if nothing has been recorded.
  std::chrono::nanoseconds Mean() const;

 private:
  // Ranges double in width with each power of two, and each power of two is
  // split into this many equal ranges. More means finer answers and more
  // memory.
  static constexpr size_t kRangesPerDoubling = 16;

  // Enough ranges to cover every 64-bit nanosecond count.
  static constexpr size_t kRanges = 61 * kRangesPerDoubling;

  // How many measurements fell in each range.
  std::array<uint64_t, kRanges> counts_ = {};
  uint64_t count_ = 0;
  std::chrono::nanoseconds max_{0};
  std::chrono::nanoseconds total_{0};
};

#endif  // PERF_HISTOGRAM_H_
