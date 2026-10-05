// LatencyHistogram by example: record measurements, then ask what was
// typical and what was unusual.

#include "perf/histogram.h"

#include <benchmark/benchmark.h>

#include <chrono>
#include <cstdint>

#include "gtest/gtest.h"

using std::chrono::microseconds;
using std::chrono::milliseconds;
using std::chrono::nanoseconds;

// Before anything is recorded, every answer is zero rather than an error.
TEST(LatencyHistogramTest, StartsEmpty) {
  const LatencyHistogram histogram;
  EXPECT_EQ(histogram.count(), 0);
  EXPECT_EQ(histogram.Percentile(50), nanoseconds(0));
  EXPECT_EQ(histogram.max(), nanoseconds(0));
  EXPECT_EQ(histogram.Mean(), nanoseconds(0));
}

// Percentiles answer "how long did most requests take?". With measurements
// of 1 to 100 microseconds, half are at or below 50 and 99% at or below 99.
TEST(LatencyHistogramTest, PercentilesDescribeTheDistribution) {
  LatencyHistogram histogram;
  for (int i = 1; i <= 100; ++i) histogram.Record(microseconds(i));

  EXPECT_EQ(histogram.count(), 100);
  // Approximate, so compared with a tolerance; see the next test.
  EXPECT_NEAR(histogram.Percentile(50).count(), 50'000, 50'000 * 0.07);
  EXPECT_NEAR(histogram.Percentile(99).count(), 99'000, 99'000 * 0.07);
  EXPECT_EQ(histogram.Percentile(100), microseconds(100));
}

// The price of fixed memory is precision: a percentile is never below the
// true value and never more than about 6% above it, at any scale.
TEST(LatencyHistogramTest, PercentilesAreWithinSixPercent) {
  for (const nanoseconds value : {
           nanoseconds(7),
           nanoseconds(1'234),
           nanoseconds(987'654),
           nanoseconds(55'555'555),
           nanoseconds(3'000'000'000),
       }) {
    LatencyHistogram histogram;
    histogram.Record(value);
    // Followed by something larger, so that the answer is not simply the max.
    histogram.Record(value * 10);
    const nanoseconds median = histogram.Percentile(50);
    EXPECT_GE(median, value);
    EXPECT_LE(median.count(), value.count() * 1.0625);
  }
}

// The maximum and the mean are tracked on the side, so they are exact.
TEST(LatencyHistogramTest, MaxAndMeanAreExact) {
  LatencyHistogram histogram;
  histogram.Record(microseconds(10));
  histogram.Record(microseconds(20));
  histogram.Record(milliseconds(3));
  EXPECT_EQ(histogram.max(), milliseconds(3));
  EXPECT_EQ(histogram.Mean(), microseconds(1010));
}

// A few slow requests barely move the median but dominate the tail, which is
// why a benchmark reports both.
TEST(LatencyHistogramTest, TailShowsWhatTheMedianHides) {
  LatencyHistogram histogram;
  for (int i = 0; i < 98; ++i) histogram.Record(microseconds(50));
  histogram.Record(milliseconds(20));
  histogram.Record(milliseconds(20));

  EXPECT_LT(histogram.Percentile(50), microseconds(60));
  EXPECT_GE(histogram.Percentile(99), milliseconds(20));
}

// Each benchmark thread records into its own histogram; merging them gives
// the same answers as if everything had been recorded in one.
TEST(LatencyHistogramTest, MergeCombinesMeasurements) {
  LatencyHistogram first;
  LatencyHistogram second;
  LatencyHistogram both;
  for (int i = 1; i <= 50; ++i) {
    first.Record(microseconds(i));
    both.Record(microseconds(i));
  }
  for (int i = 51; i <= 100; ++i) {
    second.Record(microseconds(i));
    both.Record(microseconds(i));
  }

  first.Merge(second);
  EXPECT_EQ(first.count(), both.count());
  EXPECT_EQ(first.Percentile(50), both.Percentile(50));
  EXPECT_EQ(first.Percentile(99), both.Percentile(99));
  EXPECT_EQ(first.max(), both.max());
  EXPECT_EQ(first.Mean(), both.Mean());
}

// A clock that steps backwards could produce a negative latency. It is
// counted, as zero, rather than corrupting the histogram.
TEST(LatencyHistogramTest, NegativeLatencyCountsAsZero) {
  LatencyHistogram histogram;
  histogram.Record(nanoseconds(-5));
  EXPECT_EQ(histogram.count(), 1);
  EXPECT_EQ(histogram.max(), nanoseconds(0));
}

// -----------------------------------------------------------------------------
// Benchmarks
// -----------------------------------------------------------------------------
//
// What recording a latency costs.
//
//   bazel run -c opt //perf:histogram_test -- --benchmark_filter=all
//
// The load generator records one latency per request. That has to be far
// cheaper than a request, or the benchmark would be measuring itself.
//
// clang-format off
// Results. Written by perf/record_results.py; do not edit by hand.
//
//   Date      2026-10-05
//   CPU       Intel(R) Core(TM) i7-9700 CPU @ 3.00GHz, 8 cores, L3 12 MiB (1 instance)
//   Memory    31 GB
//   Disk      Samsung SSD 990 EVO Plus 4TB (ext4)
//   System    Linux 7.0.0-38-generic, CPU governor: powersave
//   Compiler  clang version 21.1.8
//   Build     bazel -c opt (-O2), C++20, no exceptions
//   I/O       io_uring, except where a benchmark's name says epoll
//
//   --------------------------------------------------------
//   Benchmark              Time             CPU   Iterations
//   --------------------------------------------------------
//   BM_Record           3.90 ns         3.90 ns    107761767
//   BM_Percentile        101 ns          101 ns      3876063
//   BM_Merge             175 ns          175 ns      2358761
// End of results.
// clang-format on

namespace {

// A histogram holding a million latencies spread over a realistic range,
// tens of microseconds to tens of milliseconds.
LatencyHistogram FilledHistogram() {
  LatencyHistogram histogram;
  uint64_t value = 20'000;
  for (int i = 0; i < 1'000'000; ++i) {
    histogram.Record(nanoseconds(value));
    // Any cheap sequence that wanders over the range will do.
    value = 20'000 + (value * 31 + 7) % 20'000'000;
  }
  return histogram;
}

// One measurement added. Paid once per request by the load generator.
void BM_Record(benchmark::State& state) {
  LatencyHistogram histogram;
  uint64_t value = 20'000;
  for (auto _ : state) {
    histogram.Record(nanoseconds(value));
    value = 20'000 + (value * 31 + 7) % 20'000'000;
  }
  benchmark::DoNotOptimize(histogram);
}
BENCHMARK(BM_Record);

// One percentile asked for. Paid a few times per round, when printing.
void BM_Percentile(benchmark::State& state) {
  const LatencyHistogram histogram = FilledHistogram();
  for (auto _ : state) benchmark::DoNotOptimize(histogram.Percentile(99));
}
BENCHMARK(BM_Percentile);

// One thread's histogram folded into the total. Paid once per thread per
// round.
void BM_Merge(benchmark::State& state) {
  const LatencyHistogram one_thread = FilledHistogram();
  LatencyHistogram total;
  for (auto _ : state) total.Merge(one_thread);
  benchmark::DoNotOptimize(total);
}
BENCHMARK(BM_Merge);

}  // namespace
