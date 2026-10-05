#include "os/io_uring_backend.h"

#include <liburing.h>

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <memory>

#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/status/status_macros.h"
#include "absl/status/statusor.h"
#include "os/io.h"

namespace os_internal {

namespace {

// Most finished operations handled between two trips into the kernel.
constexpr unsigned kCompletionBatch = 256;

}  // namespace

class IoUringBackend final : public Backend {
 public:
  // `uring_` is not usable until Initialize succeeds.
  IoUringBackend() = default;

  ~IoUringBackend() override {
    if (initialized_) io_uring_queue_exit(&uring_);
  }

  absl::Status Initialize(const os::IoUringOptions& options) {
    io_uring_params params = {};
    // SINGLE_ISSUER promises that one thread uses the ring. R_DISABLED
    // postpones choosing that thread until OnAttach.
    params.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_R_DISABLED;
    if (options.completion_work ==
        os::IoUringOptions::CompletionWork::kWhileWaiting) {
      params.flags |= IORING_SETUP_DEFER_TASKRUN;
    }
    const int result =
        io_uring_queue_init_params(options.queue_depth, &uring_, &params);
    if (result < 0) {
      return absl::ErrnoToStatus(-result, "io_uring is not available");
    }
    initialized_ = true;
    return absl::OkStatus();
  }

  void OnAttach() override {
    QCHECK(!attached_) << "an IoDriver can only be attached once";
    attached_ = true;
    QCHECK_EQ(io_uring_enable_rings(&uring_), 0);
  }

  // io_uring can only say whether an operation is finished after the kernel
  // has been asked, so nothing finishes on the spot.
  bool TryNow(Operation&) override { return false; }

  void Start(Operation& operation) override {
    // A sleep is itself a timer; anything else with a time limit gets one
    // attached.
    const bool limited = operation.time_limit_.has_value() &&
                         operation.needs_ != Operation::Needs::kTime;
    // An operation and its time limit must be adjacent in the queue, so make
    // room for both before taking either.
    if (io_uring_sq_space_left(&uring_) < (limited ? 2U : 1U)) {
      io_uring_submit(&uring_);
    }

    io_uring_sqe* const entry = io_uring_get_sqe(&uring_);
    operation.Describe(entry);
    // After Describe, which resets the entry.
    io_uring_sqe_set_data(entry, &operation);
    if (limited) {
      // Linking makes the next entry, a timeout, apply to this one.
      entry->flags |= IOSQE_IO_LINK;
      io_uring_sqe* const limit = io_uring_get_sqe(&uring_);
      io_uring_prep_link_timeout(limit, &operation.kernel_time_limit_, 0);
      io_uring_sqe_set_data(limit, nullptr);
    }
    ++operations_in_flight_;
  }

  void WakeFinished(const bool& stop) override {
    SubmitAndWait();
    CollectFinished([&stop](Operation* operation, int result) {
      if (stop) return;
      // How the kernel reports an operation stopped by its linked timeout.
      const bool timed_out = result == -ECANCELED &&
                             operation->time_limit_.has_value() &&
                             operation->needs_ != Operation::Needs::kTime;
      operation->Complete(timed_out ? -ETIMEDOUT : result);
    });
  }

  void CancelAll() override {
    if (operations_in_flight_ > 0) {
      if (io_uring_sq_space_left(&uring_) == 0) io_uring_submit(&uring_);
      io_uring_sqe* const cancel = io_uring_get_sqe(&uring_);
      io_uring_prep_cancel(cancel, nullptr, IORING_ASYNC_CANCEL_ANY);
      io_uring_sqe_set_data(cancel, nullptr);
    }
    // The kernel may still be writing into memory owned by a suspended task,
    // so wait for it to report on every operation, cancelled or not.
    while (operations_in_flight_ > 0) {
      SubmitAndWait();
      CollectFinished([](Operation*, int) {});
    }
  }

 private:
  // Hands queued operations to the kernel and blocks until at least one
  // operation has finished.
  void SubmitAndWait() {
    const int result = io_uring_submit_and_wait(&uring_, 1);
    QCHECK(result >= 0 || result == -EINTR)
        << "io_uring stopped working: " << strerror(-result);
  }

  // Removes the finished operations from the completion queue and calls
  // `finish` with each one and the kernel's result for it.
  template <typename Finish>
  void CollectFinished(Finish finish) {
    // Copied out before calling `finish`, which may run tasks that queue and
    // collect further operations.
    struct Finished {
      Operation* operation;
      int result;
    };
    Finished finished[kCompletionBatch];
    io_uring_cqe* entries[kCompletionBatch];
    const unsigned count =
        io_uring_peek_batch_cqe(&uring_, entries, kCompletionBatch);
    for (unsigned i = 0; i < count; ++i) {
      finished[i] = {
          .operation =
              static_cast<Operation*>(io_uring_cqe_get_data(entries[i])),
          .result = entries[i]->res,
      };
    }
    io_uring_cq_advance(&uring_, count);

    for (unsigned i = 0; i < count; ++i) {
      // Time limits and cancellations are queue entries of their own, with no
      // Operation behind them.
      if (finished[i].operation == nullptr) continue;
      --operations_in_flight_;
      finish(finished[i].operation, finished[i].result);
    }
  }

  io_uring uring_;
  bool initialized_ = false;
  bool attached_ = false;

  // Operations queued or with the kernel whose results have not been
  // collected.
  size_t operations_in_flight_ = 0;
};

absl::StatusOr<std::unique_ptr<Backend>> NewIoUringBackend(
    const os::IoUringOptions& options) {
  auto backend = std::make_unique<IoUringBackend>();
  ABSL_RETURN_IF_ERROR(backend->Initialize(options));
  return backend;
}

}  // namespace os_internal
