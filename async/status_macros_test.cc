// CO_RETURN_IF_ERROR and CO_ASSIGN_OR_RETURN by example: propagating failure
// out of an asynchronous function in one line.

#include "async/status_macros.h"

#include <cstddef>
#include <memory>
#include <string>
#include <utility>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "async/task.h"
#include "async/task_scope.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace {

using absl_testing::IsOk;
using absl_testing::IsOkAndHolds;
using absl_testing::StatusIs;
using testing::Eq;
using testing::Pointee;

// Runs a task that never has to wait, and returns what it produced.
template <typename T>
T RunNow(Task<T> task) {
  T result;
  TaskScope scope;
  scope.Spawn([](Task<T> task, T& result) -> Task<> { result = co_await task; }(
                                              std::move(task), result));
  return result;
}

// Stand-ins for asynchronous operations that succeed or fail.
Task<absl::Status> Write(bool works) {
  co_return works ? absl::OkStatus() : absl::UnavailableError("peer is gone");
}

Task<absl::StatusOr<std::string>> ReadLine(bool works) {
  if (!works) co_return absl::DeadlineExceededError("peer is silent");
  co_return "hello";
}

Task<absl::Status> WriteTwice(bool first_works, int& writes) {
  CO_RETURN_IF_ERROR(co_await Write(first_works));
  ++writes;

  CO_RETURN_IF_ERROR(co_await Write(true));
  ++writes;

  co_return absl::OkStatus();
}

// On success the function carries on to the next line.
TEST(CoReturnIfErrorTest, CarriesOnWhenOk) {
  int writes = 0;
  EXPECT_THAT(RunNow(WriteTwice(true, writes)), IsOk());
  EXPECT_EQ(writes, 2);
}

// On failure the function ends there, and its caller gets the error.
TEST(CoReturnIfErrorTest, LeavesWithTheError) {
  int writes = 0;
  EXPECT_THAT(RunNow(WriteTwice(false, writes)),
              StatusIs(absl::StatusCode::kUnavailable, "peer is gone"));
  EXPECT_EQ(writes, 0);
}

Task<absl::StatusOr<size_t>> LineLength(bool works) {
  CO_ASSIGN_OR_RETURN(const std::string line, co_await ReadLine(works));

  co_return line.size();
}

// The value comes out of the StatusOr into a variable of its own, so the
// lines that follow deal with a string rather than a StatusOr of one.
TEST(CoAssignOrReturnTest, UnwrapsTheValueWhenOk) {
  EXPECT_THAT(RunNow(LineLength(true)), IsOkAndHolds(5));
}

TEST(CoAssignOrReturnTest, LeavesWithTheError) {
  EXPECT_THAT(RunNow(LineLength(false)),
              StatusIs(absl::StatusCode::kDeadlineExceeded, "peer is silent"));
}

// A function returning a plain Status can use it too; only the error is
// passed on.
Task<absl::Status> Greet(bool works, std::string& greeting) {
  CO_ASSIGN_OR_RETURN(greeting, co_await ReadLine(works));

  co_return absl::OkStatus();
}

TEST(CoAssignOrReturnTest, AssignsToAnExistingVariable) {
  std::string greeting;
  EXPECT_THAT(RunNow(Greet(true, greeting)), IsOk());
  EXPECT_EQ(greeting, "hello");

  EXPECT_THAT(RunNow(Greet(false, greeting)),
              StatusIs(absl::StatusCode::kDeadlineExceeded));
}

// The third argument says what was being attempted, as with Abseil's macro.
Task<absl::StatusOr<size_t>> LineLengthWithContext() {
  CO_ASSIGN_OR_RETURN(const std::string line, co_await ReadLine(false),
                      _.SetPrepend() << "reading the greeting: ");

  co_return line.size();
}

TEST(CoAssignOrReturnTest, AddsContextToTheError) {
  EXPECT_THAT(RunNow(LineLengthWithContext()),
              StatusIs(absl::StatusCode::kDeadlineExceeded,
                       "reading the greeting: peer is silent"));
}

// Values that can only be moved pass through intact.
Task<absl::StatusOr<std::unique_ptr<int>>> MakeNumber() {
  co_return std::make_unique<int>(7);
}

Task<absl::StatusOr<std::unique_ptr<int>>> PassNumberOn() {
  CO_ASSIGN_OR_RETURN(std::unique_ptr<int> number, co_await MakeNumber());

  co_return number;
}

TEST(CoAssignOrReturnTest, MovesMoveOnlyValues) {
  EXPECT_THAT(RunNow(PassNumberOn()), IsOkAndHolds(Pointee(Eq(7))));
}

}  // namespace
