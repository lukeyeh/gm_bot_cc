// Runs the load tests and prints what they measured.
//
//   bazel run -c opt //perf:bench
//   bazel run -c opt //perf:bench -- --events=5000 --history_days=120
//   bazel run -c opt //perf:bench -- --io_backend=epoll
//
// Each test is run twice: flat out, which says how many events a second can
// be handled, and at a steady rate well within that, which says how long an
// event waits when the system is not behind.

#include <chrono>
#include <cstdio>
#include <string>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/status/statusor.h"
#include "perf/load.h"

ABSL_FLAG(int, events, 2000, "How many events each run sends");
ABSL_FLAG(int, members, 200, "How many people the bot's events come from");
ABSL_FLAG(int, history_days, 30,
          "How many days of GMs each person has before the bot's runs start");
ABSL_FLAG(int, paced_rate, 200,
          "Events a second in the runs that are not flat out");

namespace {

double Microseconds(std::chrono::nanoseconds duration) {
  return static_cast<double>(duration.count()) / 1e3;
}

void Report(const std::string& name, const absl::StatusOr<LoadResult>& result) {
  if (!result.ok()) {
    std::printf("%-28s FAILED: %s\n", name.c_str(),
                result.status().ToString().c_str());
    return;
  }
  std::printf(
      "%-28s %7llu events %10.0f /s   p50 %9.1f us   p99 %9.1f us   "
      "max %9.1f us\n",
      name.c_str(), static_cast<unsigned long long>(result->handled),
      result->events_per_second(), Microseconds(result->latency.Percentile(50)),
      Microseconds(result->latency.Percentile(99)),
      Microseconds(result->latency.max()));
}

}  // namespace

int main(int argc, char** argv) {
  absl::ParseCommandLine(argc, argv);

  const LoadOptions flat_out{
      .events = absl::GetFlag(FLAGS_events),
      .members = absl::GetFlag(FLAGS_members),
      .history_days = absl::GetFlag(FLAGS_history_days),
  };
  LoadOptions paced = flat_out;
  paced.interval =
      std::chrono::microseconds(1'000'000 / absl::GetFlag(FLAGS_paced_rate));

  Report("websocket, flat out", RunWebSocketLoad(flat_out));
  Report("websocket, paced", RunWebSocketLoad(paced));
  Report("bot, flat out", RunBotLoad(flat_out));
  Report("bot, paced", RunBotLoad(paced));
  return 0;
}
