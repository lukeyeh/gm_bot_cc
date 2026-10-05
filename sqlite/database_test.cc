// Shows how to store and query data with sqlite::Database.

#include "sqlite/database.h"

#include <benchmark/benchmark.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace sqlite {
namespace {

using ::absl_testing::StatusIs;
using ::testing::HasSubstr;

// The everyday cycle: create a table, insert with bound parameters, read back.
TEST(DatabaseTest, StoresAndQueriesRows) {
  absl::StatusOr<Database> database = Database::InMemory();
  ABSL_ASSERT_OK(database);
  ABSL_ASSERT_OK(database->Execute(
      "CREATE TABLE members (id INTEGER PRIMARY KEY, name TEXT, score REAL)"));
  ABSL_ASSERT_OK(database->Execute("INSERT INTO members VALUES (?, ?, ?)",
                                   {
                                       int64_t{7},
                                       std::string("luke"),
                                       std::monostate(),
                                   }));
  ABSL_ASSERT_OK(database->Execute("INSERT INTO members VALUES (?, ?, ?)",
                                   {
                                       int64_t{8},
                                       std::string("it's \"quoted\""),
                                       1.5,
                                   }));

  const absl::StatusOr<std::vector<Row>> rows = database->Query(
      "SELECT id, name, score FROM members WHERE id >= ? "
      "ORDER BY id",
      {
          int64_t{7},
      });

  ABSL_ASSERT_OK(rows);
  ASSERT_EQ(rows->size(), 2);
  EXPECT_EQ((*rows)[0].Int(0), 7);
  EXPECT_EQ((*rows)[0].Text(1), "luke");
  EXPECT_TRUE((*rows)[0].IsNull(2));
  // Parameters are data, never SQL, whatever they contain.
  EXPECT_EQ((*rows)[1].Text(1), "it's \"quoted\"");
  EXPECT_FALSE((*rows)[1].IsNull(2));
}

// Rows read leniently, so a query's caller needs no per-column error handling.
TEST(DatabaseTest, RowsReadMissingOrMistypedColumnsAsZero) {
  absl::StatusOr<Database> database = Database::InMemory();
  ABSL_ASSERT_OK(database);

  const absl::StatusOr<std::vector<Row>> rows =
      database->Query("SELECT 'text', 42, NULL");

  ABSL_ASSERT_OK(rows);
  ASSERT_EQ(rows->size(), 1);
  const Row& row = rows->front();
  EXPECT_EQ(row.Int(0), 0);    // Text is not an integer.
  EXPECT_EQ(row.Text(1), "");  // Nor an integer text.
  EXPECT_EQ(row.Int(2), 0);    // NULL.
  EXPECT_EQ(row.Int(99), 0);   // No such column.
  EXPECT_TRUE(row.IsNull(99));
}

// A statement can be run again and again, each time with its own parameters
// and none left over from the last. (Statements are compiled once and kept,
// so this is the same compiled statement each time.)
TEST(DatabaseTest, RunsTheSameStatementRepeatedly) {
  absl::StatusOr<Database> database = Database::InMemory();
  ABSL_ASSERT_OK(database);
  ABSL_ASSERT_OK(database->Execute("CREATE TABLE t (k INTEGER, v TEXT)"));

  for (int64_t key = 0; key < 3; ++key) {
    ABSL_ASSERT_OK(
        database->Execute("INSERT INTO t VALUES (?, ?)", {
                                                             key,
                                                             std::string("row"),
                                                         }));
  }
  // Fewer parameters than placeholders: the rest are NULL, not whatever the
  // last run bound.
  ABSL_ASSERT_OK(
      database->Execute("INSERT INTO t VALUES (?, ?)", {
                                                           int64_t{3},
                                                       }));

  for (int64_t key = 0; key < 4; ++key) {
    const absl::StatusOr<std::vector<Row>> rows =
        database->Query("SELECT v FROM t WHERE k = ?", {
                                                           key,
                                                       });
    ABSL_ASSERT_OK(rows);
    ASSERT_EQ(rows->size(), 1);
    EXPECT_EQ(rows->front().IsNull(0), key == 3);
  }
}

// A statement that fails can be run again once what made it fail is gone.
TEST(DatabaseTest, AFailedStatementCanBeRunAgain) {
  absl::StatusOr<Database> database = Database::InMemory();
  ABSL_ASSERT_OK(database);
  ABSL_ASSERT_OK(database->Execute("CREATE TABLE t (k INTEGER PRIMARY KEY)"));
  ABSL_ASSERT_OK(database->Execute("INSERT INTO t VALUES (1)"));

  EXPECT_THAT(database->Execute("INSERT INTO t VALUES (1)"),
              StatusIs(absl::StatusCode::kAlreadyExists));
  ABSL_ASSERT_OK(database->Execute("DELETE FROM t"));
  ABSL_EXPECT_OK(database->Execute("INSERT INTO t VALUES (1)"));
}

// Kept statements survive the table they read being changed underneath them.
TEST(DatabaseTest, KeptStatementsFollowChangesToTheSchema) {
  absl::StatusOr<Database> database = Database::InMemory();
  ABSL_ASSERT_OK(database);
  ABSL_ASSERT_OK(database->Execute("CREATE TABLE t (k INTEGER)"));
  ABSL_ASSERT_OK(database->Execute("INSERT INTO t VALUES (1)"));
  ABSL_ASSERT_OK(database->Query("SELECT * FROM t"));

  ABSL_ASSERT_OK(database->Execute("ALTER TABLE t ADD COLUMN v TEXT"));

  const absl::StatusOr<std::vector<Row>> rows =
      database->Query("SELECT * FROM t");
  ABSL_ASSERT_OK(rows);
  ASSERT_EQ(rows->size(), 1);
  EXPECT_TRUE(rows->front().IsNull(1));  // The new column is there.
}

// A transaction's changes all happen, or none do.
TEST(DatabaseTest, TransactionUndoesEverythingWhenTheBodyFails) {
  absl::StatusOr<Database> database = Database::InMemory();
  ABSL_ASSERT_OK(database);
  ABSL_ASSERT_OK(database->Execute("CREATE TABLE t (x INTEGER)"));

  ABSL_ASSERT_OK(database->Transaction(
      [&] { return database->Execute("INSERT INTO t VALUES (1)"); }));
  EXPECT_THAT(database->Transaction([&] {
    database->Execute("INSERT INTO t VALUES (2)").IgnoreError();
    return absl::AbortedError("changed my mind");
  }),
              StatusIs(absl::StatusCode::kAborted));

  const absl::StatusOr<std::vector<Row>> rows =
      database->Query("SELECT COUNT(*), MAX(x) FROM t");
  ABSL_ASSERT_OK(rows);
  EXPECT_EQ(rows->front().Int(0), 1);
  EXPECT_EQ(rows->front().Int(1), 1);
}

// Failures say what SQLite said, under a code that tells the kinds apart.
TEST(DatabaseTest, ReportsFailuresBySpecificCode) {
  absl::StatusOr<Database> database = Database::InMemory();
  ABSL_ASSERT_OK(database);
  ABSL_ASSERT_OK(database->Execute(
      "CREATE TABLE t (x INTEGER PRIMARY KEY, y TEXT NOT NULL)"));
  ABSL_ASSERT_OK(database->Execute("INSERT INTO t VALUES (1, 'a')"));

  EXPECT_THAT(
      database->Execute("SELEKT 1"),
      StatusIs(absl::StatusCode::kInvalidArgument, HasSubstr("syntax error")));
  EXPECT_THAT(database->Execute("INSERT INTO t VALUES (1, 'b')"),
              StatusIs(absl::StatusCode::kAlreadyExists));
  EXPECT_THAT(database->Execute("INSERT INTO t VALUES (2, NULL)"),
              StatusIs(absl::StatusCode::kFailedPrecondition));
  EXPECT_THAT(database->Execute(""),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

// A database opened from a file keeps its contents after it is closed.
TEST(DatabaseTest, FilesPersist) {
  const std::filesystem::path file =
      std::filesystem::path(std::getenv("TEST_TMPDIR")) / "persist.db";
  {
    absl::StatusOr<Database> database = Database::Open(file);
    ABSL_ASSERT_OK(database);
    ABSL_ASSERT_OK(database->Execute("CREATE TABLE t (x TEXT)"));
    ABSL_ASSERT_OK(database->Execute("INSERT INTO t VALUES ('kept')"));
  }

  absl::StatusOr<Database> reopened = Database::Open(file);
  ABSL_ASSERT_OK(reopened);
  const absl::StatusOr<std::vector<Row>> rows =
      reopened->Query("SELECT x FROM t");
  ABSL_ASSERT_OK(rows);
  ASSERT_EQ(rows->size(), 1);
  EXPECT_EQ(rows->front().Text(0), "kept");
}

// A file is written through a log kept beside it, which is what makes
// changes cheap. Closing the database folds the log back in.
TEST(DatabaseTest, FilesAreWrittenThroughALog) {
  const std::filesystem::path file =
      std::filesystem::path(std::getenv("TEST_TMPDIR")) / "logged.db";
  absl::StatusOr<Database> database = Database::Open(file);
  ABSL_ASSERT_OK(database);
  ABSL_ASSERT_OK(database->Execute("CREATE TABLE t (x TEXT)"));

  const absl::StatusOr<std::vector<Row>> mode =
      database->Query("PRAGMA journal_mode");
  ABSL_ASSERT_OK(mode);
  EXPECT_EQ(mode->front().Text(0), "wal");
  EXPECT_TRUE(std::filesystem::exists(file.string() + "-wal"));
}

TEST(DatabaseTest, OpenFailsWhereNoFileCanBe) {
  EXPECT_THAT(Database::Open("/nonexistent-directory/gm.db"),
              StatusIs(absl::StatusCode::kNotFound));
}

// -----------------------------------------------------------------------------
// Benchmarks
// -----------------------------------------------------------------------------
//
//   bazel run -c opt //sqlite:database_test -- --benchmark_filter=all
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
//   ----------------------------------------------------------
//   Benchmark                Time             CPU   Iterations
//   ----------------------------------------------------------
//   BM_Insert             1011 ns         1010 ns       424973
//   BM_SelectByKey         560 ns          559 ns       772035
//   BM_InsertToFile       9071 ns         5143 ns        80305
// End of results.
// clang-format on

// One row in, by itself. Every statement outside a transaction is its own.
void BM_Insert(benchmark::State& state) {
  absl::StatusOr<Database> database = Database::InMemory();
  database->Execute("CREATE TABLE t (k INTEGER PRIMARY KEY, v TEXT)")
      .IgnoreError();
  int64_t key = 0;
  for (auto _ : state) {
    database
        ->Execute("INSERT INTO t VALUES (?, ?)",
                  {
                      key++,
                      std::string("value"),
                  })
        .IgnoreError();
  }
}
BENCHMARK(BM_Insert);

// One row out by its key, from a table of ten thousand.
void BM_SelectByKey(benchmark::State& state) {
  absl::StatusOr<Database> database = Database::InMemory();
  database->Execute("CREATE TABLE t (k INTEGER PRIMARY KEY, v TEXT)")
      .IgnoreError();
  database
      ->Transaction([&] {
        for (int64_t key = 0; key < 10'000; ++key) {
          database
              ->Execute("INSERT INTO t VALUES (?, ?)",
                        {
                            key,
                            std::string("value"),
                        })
              .IgnoreError();
        }
        return absl::OkStatus();
      })
      .IgnoreError();

  int64_t key = 0;
  for (auto _ : state) {
    benchmark::DoNotOptimize(
        database->Query("SELECT v FROM t WHERE k = ?", {
                                                           key++ % 10'000,
                                                       }));
  }
}
BENCHMARK(BM_SelectByKey);

// The same insert, to a file: what durability costs.
void BM_InsertToFile(benchmark::State& state) {
  const std::filesystem::path file =
      std::filesystem::path(std::getenv("TEST_TMPDIR") != nullptr
                                ? std::getenv("TEST_TMPDIR")
                                : "/tmp") /
      "bench_insert.db";
  std::filesystem::remove(file);
  absl::StatusOr<Database> database = Database::Open(file);
  database->Execute("CREATE TABLE t (k INTEGER PRIMARY KEY, v TEXT)")
      .IgnoreError();
  int64_t key = 0;
  for (auto _ : state) {
    database
        ->Execute("INSERT INTO t VALUES (?, ?)",
                  {
                      key++,
                      std::string("value"),
                  })
        .IgnoreError();
  }
}
BENCHMARK(BM_InsertToFile);

}  // namespace
}  // namespace sqlite
