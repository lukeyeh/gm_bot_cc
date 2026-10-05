#include "os/epoll_backend.h"

#include <sys/epoll.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "os/io.h"

namespace os_internal {

namespace {

using Clock = std::chrono::steady_clock;

// Most ready sockets handled between two trips into the kernel.
constexpr int kEventBatch = 256;

}  // namespace

class EpollBackend final : public Backend {
 public:
  explicit EpollBackend(int epoll) : epoll_(epoll) {}
  ~EpollBackend() override { close(epoll_); }

  void OnAttach() override {
    QCHECK(!attached_) << "an IoDriver can only be attached once";
    attached_ = true;
  }

  // Most operations finish here: data is usually already waiting to be read,
  // and there is usually room to write.
  bool TryNow(Operation& operation) override {
    if (operation.needs_ == Operation::Needs::kTime) return false;
    const int result = operation.Attempt();
    if (result == -EAGAIN) return false;
    operation.result_ = result;
    return true;
  }

  void Start(Operation& operation) override {
    if (operation.time_limit_.has_value()) {
      operation.deadline_ = Clock::now() + *operation.time_limit_;
      AddToTimers(operation);
    }
    if (operation.needs_ != Operation::Needs::kTime) Park(operation);
  }

  void WakeFinished(const bool& stop) override {
    epoll_event events[kEventBatch];
    const int count =
        epoll_wait(epoll_, events, kEventBatch, MillisecondsUntilNextTimer());
    QCHECK(count >= 0 || errno == EINTR) << "epoll stopped working";

    // Unpark the operations whose sockets are ready before retrying any of
    // them, because finishing one runs a task, which may park others.
    Operation* ready[2 * kEventBatch];
    size_t ready_count = 0;
    for (int i = 0; i < count; ++i) {
      Parked& parked = parked_[events[i].data.fd];
      // An error or a hang-up is news for both directions: the operation's
      // next attempt is what reports it.
      const bool failed = (events[i].events & (EPOLLERR | EPOLLHUP)) != 0;
      if (parked.reader != nullptr &&
          (failed || (events[i].events & (EPOLLIN | EPOLLRDHUP)) != 0)) {
        ready[ready_count++] = std::exchange(parked.reader, nullptr);
      }
      if (parked.writer != nullptr &&
          (failed || (events[i].events & EPOLLOUT) != 0)) {
        ready[ready_count++] = std::exchange(parked.writer, nullptr);
      }
    }

    for (size_t i = 0; i < ready_count; ++i) {
      if (stop) return;
      Operation& operation = *ready[i];
      const int result = operation.Attempt();
      if (result == -EAGAIN) {
        // Ready for someone else, or not as ready as it looked.
        Park(operation);
        continue;
      }
      RemoveFromTimers(operation);
      operation.Complete(result);
    }

    // Finishing an operation can add timers, so look at the list afresh each
    // time round.
    const Clock::time_point now = Clock::now();
    while (!stop && soonest_ != nullptr && soonest_->deadline_ <= now) {
      Operation& operation = *soonest_;
      RemoveFromTimers(operation);
      Unpark(operation);
      // Running out of time is how a sleep succeeds and how anything else
      // fails.
      operation.Complete(
          operation.needs_ == Operation::Needs::kTime ? 0 : -ETIMEDOUT);
    }
  }

  // The kernel never refers to an operation's memory under epoll, so
  // forgetting the operations is all it takes.
  void CancelAll() override {
    std::fill(parked_.begin(), parked_.end(), Parked{});
    while (soonest_ != nullptr) RemoveFromTimers(*soonest_);
  }

 private:
  // The operations waiting on one socket: at most one for each direction.
  struct Parked {
    Operation* reader = nullptr;
    Operation* writer = nullptr;
  };

  static bool IsReader(const Operation& operation) {
    return operation.needs_ == Operation::Needs::kReadable ||
           operation.needs_ == Operation::Needs::kIncomingConnection;
  }

  // Records that `operation` is waiting for its socket, and makes sure epoll
  // is watching that socket.
  void Park(Operation& operation) {
    const size_t descriptor = static_cast<size_t>(operation.descriptor_);
    if (descriptor >= parked_.size()) parked_.resize(descriptor + 1);
    Operation*& slot = IsReader(operation) ? parked_[descriptor].reader
                                           : parked_[descriptor].writer;
    QCHECK(slot == nullptr)
        << "two operations of the same kind awaited on one socket at once";
    slot = &operation;

    // Edge-triggered: epoll reports a socket once each time it becomes ready,
    // not for as long as it stays ready. An operation is only parked after
    // an attempt has found the socket not ready, so no report can be missed.
    //
    // A listening socket may be watched by several threads. EPOLLEXCLUSIVE
    // has epoll wake one of them per connection instead of all.
    epoll_event event = {};
    event.data.fd = operation.descriptor_;
    event.events = operation.needs_ == Operation::Needs::kIncomingConnection
                       ? EPOLLIN | EPOLLET | EPOLLEXCLUSIVE
                       : EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET;
    // Adding every time, and ignoring "already there", is one system call
    // either way and needs no record of which sockets are being watched. Such
    // a record would go stale whenever a socket is closed and its number
    // reused, since the kernel stops watching a closed socket by itself.
    const int result =
        epoll_ctl(epoll_, EPOLL_CTL_ADD, operation.descriptor_, &event);
    QCHECK(result == 0 || errno == EEXIST) << "cannot watch socket with epoll";
  }

  void Unpark(Operation& operation) {
    if (operation.needs_ == Operation::Needs::kTime) return;
    Parked& parked = parked_[operation.descriptor_];
    if (parked.reader == &operation) parked.reader = nullptr;
    if (parked.writer == &operation) parked.writer = nullptr;
  }

  // Inserts `operation` into the list of timed operations, which is kept in
  // order of deadline. Nearly every time limit is the same length, so a new
  // operation nearly always belongs at the end, and searching from there is
  // quick.
  void AddToTimers(Operation& operation) {
    Operation* before = latest_;
    while (before != nullptr && before->deadline_ > operation.deadline_) {
      before = before->sooner_;
    }
    Operation* const after = before == nullptr ? soonest_ : before->later_;
    operation.sooner_ = before;
    operation.later_ = after;
    (before == nullptr ? soonest_ : before->later_) = &operation;
    (after == nullptr ? latest_ : after->sooner_) = &operation;
    operation.timed_ = true;
  }

  void RemoveFromTimers(Operation& operation) {
    if (!operation.timed_) return;
    (operation.sooner_ == nullptr ? soonest_ : operation.sooner_->later_) =
        operation.later_;
    (operation.later_ == nullptr ? latest_ : operation.later_->sooner_) =
        operation.sooner_;
    operation.sooner_ = nullptr;
    operation.later_ = nullptr;
    operation.timed_ = false;
  }

  // How long epoll may wait before a time limit needs attention, in the form
  // epoll_wait takes: milliseconds, or -1 for no limit.
  int MillisecondsUntilNextTimer() const {
    if (soonest_ == nullptr) return -1;
    const auto remaining = soonest_->deadline_ - Clock::now();
    if (remaining <= Clock::duration::zero()) return 0;
    // Rounded up, so as not to wake just before the deadline and spin.
    return static_cast<int>(
        std::chrono::ceil<std::chrono::milliseconds>(remaining).count());
  }

  int epoll_;
  bool attached_ = false;

  // Parked operations, indexed by their socket's descriptor.
  std::vector<Parked> parked_;

  // The ends of the list of operations with a time limit, ordered by
  // deadline.
  Operation* soonest_ = nullptr;
  Operation* latest_ = nullptr;
};

absl::StatusOr<std::unique_ptr<Backend>> NewEpollBackend() {
  const int epoll = epoll_create1(EPOLL_CLOEXEC);
  if (epoll < 0) return absl::ErrnoToStatus(errno, "cannot set up epoll");
  return std::make_unique<EpollBackend>(epoll);
}

}  // namespace os_internal
