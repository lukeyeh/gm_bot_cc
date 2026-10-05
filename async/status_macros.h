// ABSL_RETURN_IF_ERROR and ABSL_ASSIGN_OR_RETURN for asynchronous functions.
//
// Abseil's macros leave a function with `return`, which a coroutine may not
// use. These are the same macros leaving with `co_return`, for use in a
// function that returns Task<absl::Status> or Task<absl::StatusOr<T>>:
//
//   Task<absl::StatusOr<Reply>> Ask(Stream& stream, std::string_view question)
//   {
//     CO_RETURN_IF_ERROR(co_await stream.Write(question));
//
//     CO_ASSIGN_OR_RETURN(const std::string line, co_await ReadLine(stream));
//     CO_ASSIGN_OR_RETURN(Reply reply, ParseReply(line),
//                         _.SetPrepend() << "bad reply: ");
//     co_return reply;
//   }
//
// Everything said about the originals in absl/status/status_macros.h holds
// for these, including the optional third argument that adds context to the
// error on its way out.

#ifndef ASYNC_STATUS_MACROS_H_
#define ASYNC_STATUS_MACROS_H_

#include "absl/status/status_macros.h"  // IWYU pragma: export

// Abseil implements its two macros in terms of a pair that take the keyword
// to leave with. Those are internal names: if an Abseil upgrade renames
// them, this is the one place to follow.

// Evaluates `expr`, an absl::Status. If it is an error, leaves the coroutine
// with it; otherwise carries on.
#define CO_RETURN_IF_ERROR(expr) \
  ABSL_INTERNAL_STATUS_MACROS_RETURN_IF_ERROR_IMPL_(co_return, expr)

// CO_ASSIGN_OR_RETURN(lhs, rexpr) evaluates `rexpr`, an absl::StatusOr<T>.
// If it is an error, leaves the coroutine with it; otherwise moves its value
// into `lhs`, which may declare a variable.
//
// CO_ASSIGN_OR_RETURN(lhs, rexpr, error_expression) leaves instead with
// `error_expression`, in which `_` is an absl::StatusBuilder holding the
// error.
#define CO_ASSIGN_OR_RETURN(...) \
  ABSL_INTERNAL_STATUS_MACROS_ASSIGN_OR_RETURN_IMPL_(co_return, __VA_ARGS__)

#endif  // ASYNC_STATUS_MACROS_H_
