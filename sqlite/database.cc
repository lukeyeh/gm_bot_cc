#include "sqlite/database.h"

#include <sqlite3.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "absl/cleanup/cleanup.h"
#include "absl/functional/function_ref.h"
#include "absl/status/status.h"
#include "absl/status/status_macros.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"

namespace sqlite {
namespace {

// How long to wait for another process to let go of the database.
constexpr int kBusyTimeoutMs = 5000;

// The status for the failure `handle` last reported.
absl::Status Failure(sqlite3* const handle, const std::string_view doing) {
  const std::string message = absl::StrCat(doing, ": ", sqlite3_errmsg(handle));

  // A clash with an existing row is told apart from other constraints.
  switch (sqlite3_extended_errcode(handle)) {
    case SQLITE_CONSTRAINT_PRIMARYKEY:
    case SQLITE_CONSTRAINT_UNIQUE: return absl::AlreadyExistsError(message);
    default: break;
  }

  switch (sqlite3_errcode(handle)) {
    case SQLITE_CONSTRAINT: return absl::FailedPreconditionError(message);
    case SQLITE_BUSY:
    case SQLITE_LOCKED: return absl::UnavailableError(message);
    case SQLITE_CANTOPEN: return absl::NotFoundError(message);
    case SQLITE_PERM:
    case SQLITE_READONLY: return absl::PermissionDeniedError(message);
    case SQLITE_ERROR:
    case SQLITE_RANGE:
    case SQLITE_MISUSE: return absl::InvalidArgumentError(message);
    default: return absl::InternalError(message);
  }
}

int Bind(sqlite3_stmt* const statement, const int index, const Value& value) {
  struct Binder {
    sqlite3_stmt* statement;
    int index;

    int operator()(std::monostate) const {
      return sqlite3_bind_null(statement, index);
    }
    int operator()(const int64_t integer) const {
      return sqlite3_bind_int64(statement, index, integer);
    }
    int operator()(const double real) const {
      return sqlite3_bind_double(statement, index, real);
    }
    int operator()(const std::string& text) const {
      // SQLITE_TRANSIENT, which asks SQLite to take a copy, is an integer
      // dressed as a pointer.
      // NOLINTBEGIN(performance-no-int-to-ptr)
      return sqlite3_bind_text64(statement, index, text.data(), text.size(),
                                 SQLITE_TRANSIENT, SQLITE_UTF8);
      // NOLINTEND(performance-no-int-to-ptr)
    }
  };
  return std::visit(
      Binder{
          .statement = statement,
          .index = index,
      },
      value);
}

Value Column(sqlite3_stmt* const statement, const int index) {
  switch (sqlite3_column_type(statement, index)) {
    case SQLITE_INTEGER: return sqlite3_column_int64(statement, index);
    case SQLITE_FLOAT: return sqlite3_column_double(statement, index);
    case SQLITE_NULL: return std::monostate();
    default: {
      // Text, or a blob read as bytes.
      const void* const bytes = sqlite3_column_blob(statement, index);
      const int size = sqlite3_column_bytes(statement, index);
      return std::string(static_cast<const char*>(bytes),
                         static_cast<size_t>(size));
    }
  }
}

}  // namespace

int64_t Row::Int(const size_t column) const {
  if (column >= columns_.size()) {
    return 0;
  }

  const int64_t* const value = std::get_if<int64_t>(&columns_[column]);
  return value != nullptr ? *value : 0;
}

std::string_view Row::Text(const size_t column) const {
  if (column >= columns_.size()) {
    return {};
  }

  const std::string* const value = std::get_if<std::string>(&columns_[column]);
  return value != nullptr ? std::string_view(*value) : std::string_view();
}

bool Row::IsNull(const size_t column) const {
  return column >= columns_.size() ||
         std::holds_alternative<std::monostate>(columns_[column]);
}

void Database::Finalizer::operator()(sqlite3_stmt* const statement) const {
  sqlite3_finalize(statement);
}

void Database::Closer::operator()(sqlite3* const handle) const {
  sqlite3_close(handle);
}

absl::StatusOr<Database> Database::OpenNamed(const std::string& name) {
  sqlite3* handle = nullptr;
  const int result = sqlite3_open(name.c_str(), &handle);
  Database database(handle);  // Owns the handle even if opening failed.
  if (result != SQLITE_OK) {
    return Failure(handle, absl::StrCat("opening ", name));
  }

  sqlite3_busy_timeout(handle, kBusyTimeoutMs);
  ABSL_RETURN_IF_ERROR(database.Execute("PRAGMA foreign_keys = ON"));
  return database;
}

absl::StatusOr<Database> Database::Open(const std::filesystem::path& file) {
  ABSL_ASSIGN_OR_RETURN(Database database, OpenNamed(file.string()));

  // Changes go to a log that is folded into the file from time to time, and
  // the disk is waited on at those times rather than after every change. See
  // the header for what that promises.
  ABSL_RETURN_IF_ERROR(database.Execute("PRAGMA journal_mode = WAL"));
  ABSL_RETURN_IF_ERROR(database.Execute("PRAGMA synchronous = NORMAL"));
  return database;
}

absl::StatusOr<Database> Database::InMemory() { return OpenNamed(":memory:"); }

absl::StatusOr<sqlite3_stmt*> Database::Compiled(const std::string_view sql) {
  const auto kept = statements_.find(sql);
  if (kept != statements_.end()) return kept->second.get();

  // SQLite fills this in, so it cannot point to something const.
  sqlite3_stmt* compiled = nullptr;  // NOLINT(misc-const-correctness)
  // PERSISTENT tells SQLite the statement will be kept and reused.
  if (sqlite3_prepare_v3(
          handle_.get(), sql.data(), static_cast<int>(sql.size()),
          SQLITE_PREPARE_PERSISTENT, &compiled, nullptr) != SQLITE_OK) {
    return Failure(handle_.get(), "preparing statement");
  }
  if (compiled == nullptr) {
    return absl::InvalidArgumentError("no SQL statement to run");
  }

  statements_.emplace(sql, compiled);
  return compiled;
}

absl::StatusOr<std::vector<Row>> Database::Query(
    const std::string_view sql, const std::initializer_list<Value> parameters) {
  sqlite3* const handle = handle_.get();
  ABSL_ASSIGN_OR_RETURN(sqlite3_stmt* const statement, Compiled(sql));

  // However this ends, the statement is left ready for its next use, holding
  // neither a place in the database nor this call's parameters.
  const absl::Cleanup rewind = [statement] {
    sqlite3_reset(statement);
    sqlite3_clear_bindings(statement);
  };

  int index = 1;
  for (const Value& parameter : parameters) {
    if (Bind(statement, index++, parameter) != SQLITE_OK) {
      return Failure(handle, "binding parameter");
    }
  }

  std::vector<Row> rows;
  const int column_count = sqlite3_column_count(statement);
  while (true) {
    const int result = sqlite3_step(statement);
    if (result == SQLITE_DONE) {
      return rows;
    }
    if (result != SQLITE_ROW) {
      return Failure(handle, "running statement");
    }

    std::vector<Value> columns;
    columns.reserve(static_cast<size_t>(column_count));
    for (int column = 0; column < column_count; ++column) {
      columns.push_back(Column(statement, column));
    }
    rows.emplace_back(std::move(columns));
  }
}

absl::Status Database::Execute(const std::string_view sql,
                               const std::initializer_list<Value> parameters) {
  return Query(sql, parameters).status();
}

absl::Status Database::Transaction(
    const absl::FunctionRef<absl::Status()> body) {
  // IMMEDIATE takes the write lock up front, so the body cannot get half way
  // and then find another writer in its way.
  ABSL_RETURN_IF_ERROR(Execute("BEGIN IMMEDIATE"));

  absl::Status outcome = body();
  if (!outcome.ok()) {
    Execute("ROLLBACK").IgnoreError();
    return outcome;
  }

  return Execute("COMMIT");
}

}  // namespace sqlite
