// SQLite databases in C++ terms: open one, run SQL with bound parameters, get
// rows back. This is the only package that touches the SQLite C API;
// statements are prepared, stepped and finalised inside it, and every failure
// is an absl::Status carrying SQLite's own explanation.

#ifndef SQLITE_DATABASE_H_
#define SQLITE_DATABASE_H_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/functional/function_ref.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"

struct sqlite3;
struct sqlite3_stmt;

namespace sqlite {

// A value going into or coming out of the database. `monostate` is NULL.
using Value = std::variant<std::monostate, int64_t, double, std::string>;

// One row of a query's result.
class Row {
 public:
  explicit Row(std::vector<Value> columns) : columns_(std::move(columns)) {}

  // The value in `column`, counting from 0, as the type asked for. NULL, a
  // value of another type, or a column that does not exist reads as 0 or "".
  int64_t Int(size_t column) const;
  std::string_view Text(size_t column) const;

  bool IsNull(size_t column) const;

 private:
  std::vector<Value> columns_;
};

class Database {
 public:
  // Opens the database in `file`, creating it if it does not exist. Fails with
  // kNotFound if the file cannot be opened or created.
  //
  // What a change is promised once it has been made: it survives the program
  // crashing or being killed, always. If the whole machine loses power, the
  // last few changes before that may be lost, but the database is never left
  // damaged or half-changed. That is the trade SQLite's write-ahead log
  // offers for not waiting on the disk after every change, which is hundreds
  // of times slower. The log is kept beside the file, as `file`-wal and
  // `file`-shm.
  static absl::StatusOr<Database> Open(const std::filesystem::path& file);

  // Opens a database that lives in memory and vanishes with this object.
  static absl::StatusOr<Database> InMemory();

  // Runs one SQL statement, with `parameters` bound in order to its `?`
  // placeholders, and returns the rows it produces (none, for statements that
  // only change things). Fails with kInvalidArgument for SQL that does not
  // compile, kAlreadyExists or kFailedPrecondition when a constraint forbids
  // the change, and kUnavailable if another process holds the database.
  //
  // Each distinct `sql` is compiled the first time it is run and kept, so
  // running it again costs only the running. Put what varies in parameters
  // rather than in the text.
  absl::StatusOr<std::vector<Row>> Query(
      std::string_view sql, std::initializer_list<Value> parameters = {});

  // Query, for statements whose rows are of no interest.
  absl::Status Execute(std::string_view sql,
                       std::initializer_list<Value> parameters = {});

  // Runs `body` so that the changes it makes happen together or not at all:
  // if it returns an error, they are undone and the error is returned.
  absl::Status Transaction(absl::FunctionRef<absl::Status()> body);

 private:
  struct Closer {
    void operator()(sqlite3* handle) const;
  };
  struct Finalizer {
    void operator()(sqlite3_stmt* statement) const;
  };

  explicit Database(sqlite3* handle) : handle_(handle) {}

  static absl::StatusOr<Database> OpenNamed(const std::string& name);

  // The compiled form of `sql`, from statements_ if it has been run before.
  absl::StatusOr<sqlite3_stmt*> Compiled(std::string_view sql);

  std::unique_ptr<sqlite3, Closer> handle_;

  // Every statement run so far, compiled, by its text. Declared after the
  // handle so as to be destroyed before it: a database cannot be closed with
  // statements outstanding.
  absl::flat_hash_map<std::string, std::unique_ptr<sqlite3_stmt, Finalizer>>
      statements_;
};

}  // namespace sqlite

#endif  // SQLITE_DATABASE_H_
