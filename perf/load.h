// Load tests: how the WebSocket client, and the whole bot, keep up when
// events arrive faster than one at a time.
//
// Both tests run a stand-in for Discord's gateway on a thread of its own,
// which sends events over a real (loopback) WebSocket as a server would, and
// measure from the moment each event is sent to the moment it has been dealt
// with. Unlike the benchmarks in the _test.cc files, which time one operation
// in isolation, these include the kernel, the event loop, and the queueing
// that happens when events come in bursts.

#ifndef PERF_LOAD_H_
#define PERF_LOAD_H_

#include <chrono>
#include <cstdint>

#include "absl/status/statusor.h"
#include "perf/histogram.h"

struct LoadOptions {
  // How many events to send.
  int events = 2000;

  // How long the server waits between one event and the next. Zero sends
  // them as fast as the connection takes them, which measures throughput;
  // anything longer measures how long each waits at that rate.
  std::chrono::microseconds interval{0};

  // For the bot only: how many different people the events come from, and
  // how many days of GMs each already has in the ledger when the test
  // starts. More of either means more for the ledger to look through.
  int members = 200;
  int history_days = 0;
};

struct LoadResult {
  // How many events were dealt with: all of them, unless something broke.
  uint64_t handled = 0;

  // From the first event being sent to the last being dealt with.
  std::chrono::nanoseconds elapsed{0};

  // For each event, from its being sent to its being dealt with.
  LatencyHistogram latency;

  double events_per_second() const {
    return elapsed.count() == 0 ? 0
                                : static_cast<double>(handled) * 1e9 /
                                      static_cast<double>(elapsed.count());
  }
};

// Sends message-sized events to a WebSocket client, which does nothing with
// them but receive them. An event is dealt with when Receive returns it.
absl::StatusOr<LoadResult> RunWebSocketLoad(const LoadOptions& options);

// Sends a morning's worth of events to the whole bot: mostly GMs, some other
// chatter in the GM channel, some commands. The ledger is a file, as in
// production; only Discord's HTTP API is absent, answered from memory. An
// event is dealt with when the bot makes the API call that answers it: the
// reaction to a message, or the reply to a command.
absl::StatusOr<LoadResult> RunBotLoad(const LoadOptions& options);

#endif  // PERF_LOAD_H_
