// Shows how to parse, read, build and serialize JSON with json::Value.

#include "json/json.h"

#include <benchmark/benchmark.h>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace json {
namespace {

using ::absl_testing::StatusIs;
using ::testing::HasSubstr;

// The typical use: parse a payload and pick fields out of it.
TEST(ParseTest, ReadsNestedDocuments) {
  const absl::StatusOr<Value> document = Parse(R"({
    "op": 0,
    "t": "MESSAGE_CREATE",
    "d": {"content": "gm", "pinned": false, "mentions": [{"id": "7"}]}
  })");
  ABSL_ASSERT_OK(document);

  const Value& message = (*document)["d"];
  EXPECT_EQ((*document)["op"].AsInt(), 0);
  EXPECT_EQ((*document)["t"].AsString(), "MESSAGE_CREATE");
  EXPECT_EQ(message["content"].AsString(), "gm");
  EXPECT_FALSE(message["pinned"].AsBool());
  ASSERT_EQ(message["mentions"].items().size(), 1);
  EXPECT_EQ(message["mentions"].items()[0]["id"].AsString(), "7");
}

// Reading never fails: what is not there reads as nothing. This is what lets
// callers chain lookups through optional parts of a document.
TEST(ValueTest, MissingOrMistypedValuesReadAsZero) {
  const absl::StatusOr<Value> document = Parse(R"({"name": "luke"})");
  ABSL_ASSERT_OK(document);

  EXPECT_TRUE((*document)["member"]["nick"].is_null());
  EXPECT_EQ((*document)["member"]["nick"].AsString(), "");
  EXPECT_EQ((*document)["name"].AsInt(), 0);
  EXPECT_FALSE((*document)["name"].AsBool());
  EXPECT_TRUE((*document)["name"].items().empty());
}

// Integers stay exact; integers and reals read as each other.
TEST(ParseTest, ReadsNumbers) {
  const absl::StatusOr<Value> document =
      Parse(R"([9007199254740993, -2, 41.25, 1e3])");
  ABSL_ASSERT_OK(document);
  const std::span<const Value> numbers = document->items();

  EXPECT_EQ(numbers[0].AsInt(), 9007199254740993);  // More than a double holds.
  EXPECT_EQ(numbers[1].AsInt(), -2);
  EXPECT_DOUBLE_EQ(numbers[2].AsDouble(), 41.25);
  EXPECT_EQ(numbers[2].AsInt(), 41);
  EXPECT_DOUBLE_EQ(numbers[3].AsDouble(), 1000);
  EXPECT_DOUBLE_EQ(numbers[1].AsDouble(), -2);
}

// Escapes become the characters they stand for, as UTF-8.
TEST(ParseTest, DecodesStringEscapes) {
  const absl::StatusOr<Value> document =
      Parse(R"(["a\"b\\c\n", "é", "🌅", "🌅"])");
  ABSL_ASSERT_OK(document);
  const std::span<const Value> strings = document->items();

  EXPECT_EQ(strings[0].AsString(), "a\"b\\c\n");
  EXPECT_EQ(strings[1].AsString(), "é");
  EXPECT_EQ(strings[2].AsString(), "🌅");  // A surrogate pair.
  EXPECT_EQ(strings[3].AsString(), "🌅");  // Raw UTF-8 passes through.
}

// Text that is not JSON is rejected, with the place it went wrong.
TEST(ParseTest, RejectsMalformedText) {
  for (const char* const text : {
           "",
           "{",
           R"({"a" 1})",
           R"({"a": 1,})",
           "[1 2]",
           R"("unterminated)",
           R"("bad \q escape")",
           "nul",
           "{} extra",
       }) {
    EXPECT_THAT(Parse(text), StatusIs(absl::StatusCode::kInvalidArgument))
        << text;
  }
  EXPECT_THAT(Parse("[1, ?]"), StatusIs(absl::StatusCode::kInvalidArgument,
                                        HasSubstr("offset 4")));
}

// Hostile nesting is an error rather than a stack overflow.
TEST(ParseTest, RejectsAbsurdNesting) {
  EXPECT_THAT(Parse(std::string(100000, '[')),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

// Documents are built by chaining Set and Append; scalars convert implicitly.
TEST(SerializeTest, WritesWhatWasBuilt) {
  const Value payload = Value().Set("op", 2).Set(
      "d", Value()
               .Set("token", "secret")
               .Set("compress", false)
               .Set("shard", Value().Append(0).Append(1))
               .Set("presence", Value()));

  EXPECT_EQ(
      Serialize(payload),
      R"({"op":2,"d":{"token":"secret","compress":false,"shard":[0,1],"presence":null}})");
}

// Setting a member twice keeps the last value rather than writing it twice.
TEST(SerializeTest, SetReplacesAnExistingMember) {
  EXPECT_EQ(Serialize(Value().Set("a", 1).Set("a", 2)), R"({"a":2})");
}

// Whoever has all of an object's members or an array's elements in hand can
// hand them over at once.
TEST(ValueTest, IsBuiltFromWholeContainers) {
  std::vector<std::pair<std::string, Value>> members;
  members.emplace_back("op", 1);
  members.emplace_back("d", Value::Array({Value("a"), Value(2)}));

  EXPECT_EQ(Serialize(Value::Object(std::move(members))),
            R"({"op":1,"d":["a",2]})");
}

// Take lifts part of a document out without copying it, leaving the rest.
TEST(ValueTest, TakeRemovesAndReturnsAMember) {
  absl::StatusOr<Value> dispatch =
      Parse(R"({"op":0,"d":{"content":"gm"},"s":7})");
  ABSL_ASSERT_OK(dispatch);

  const Value data = dispatch->Take("d");

  EXPECT_EQ(data["content"].AsString(), "gm");
  EXPECT_EQ(Serialize(*dispatch), R"({"op":0,"s":7})");
  // What is not there to take is null, as with reading.
  EXPECT_TRUE(dispatch->Take("d").is_null());
  EXPECT_TRUE(Value(3).Take("d").is_null());
}

// JSON allows an object to repeat a name, and says not to. If one does, the
// first is the one found.
TEST(ParseTest, FindsTheFirstOfARepeatedName) {
  const absl::StatusOr<Value> document = Parse(R"({"a":1,"a":2})");
  ABSL_ASSERT_OK(document);

  EXPECT_EQ((*document)["a"].AsInt(), 1);
}

// Empty containers have to be asked for by name, since a fresh Value is null.
TEST(SerializeTest, WritesEmptyContainers) {
  EXPECT_EQ(Serialize(Value()), "null");
  EXPECT_EQ(Serialize(Value::Object()), "{}");
  EXPECT_EQ(Serialize(Value::Array()), "[]");
}

// Whatever a string holds, the output is valid JSON.
TEST(SerializeTest, EscapesStrings) {
  EXPECT_EQ(Serialize(Value("say \"gm\"\n\tnow\\ \x01")),
            R"("say \"gm\"\n\tnow\\ \u0001")");
  EXPECT_EQ(Serialize(Value("🌅")), "\"🌅\"");
}

// Serializing and parsing are inverses.
TEST(SerializeTest, RoundTrips) {
  const Value original = Value()
                             .Set("pi", 3.141592653589793)
                             .Set("big", int64_t{1} << 62)
                             .Set("list", Value().Append("x").Append(Value()))
                             .Set("empty", Value::Object());

  const absl::StatusOr<Value> reparsed = Parse(Serialize(original));
  ABSL_ASSERT_OK(reparsed);
  EXPECT_EQ(*reparsed, original);
}

// -----------------------------------------------------------------------------
// Benchmarks
// -----------------------------------------------------------------------------
//
//   bazel run -c opt //json:json_test -- --benchmark_filter=all
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
//   -------------------------------------------------------------------------------
//   Benchmark                     Time             CPU   Iterations UserCounters...
//   -------------------------------------------------------------------------------
//   BM_Parse                   3596 ns         3596 ns       117908 bytes_per_second=272.379Mi/s
//   BM_Lookup                  66.5 ns         66.5 ns      6461373
//   BM_BuildAndSerialize        239 ns          239 ns      1842228
//   BM_Serialize               2204 ns         2204 ns       194774
// End of results.
// clang-format on

// A gateway dispatch about the size and shape of a real one.
constexpr std::string_view kDispatch =
    R"({"t":"MESSAGE_CREATE","s":42,"op":0,"d":{"type":0,"tts":false,
  "timestamp":"2026-10-05T12:00:00.000000+00:00","pinned":false,"nonce":"2000000000000000002",
  "mentions":[],"mention_roles":[],"mention_everyone":false,
  "member":{"roles":["500000000000000001","500000000000000002"],"premium_since":null,
    "pending":false,"nick":"luke the early","mute":false,"joined_at":"2021-12-29T20:14:27.585000+00:00",
    "flags":0,"deaf":false,"communication_disabled_until":null,"banner":null,"avatar":null},
  "id":"1000000000000000001","flags":0,"embeds":[],"edited_timestamp":null,
  "content":"gm everyone, hope the coffee is strong today ☕","components":[],
  "channel_type":0,"channel_id":"2000000000000000002",
  "author":{"username":"lukeyeh","public_flags":0,"primary_guild":null,
    "id":"400000000000000004","global_name":"Luke","discriminator":"0",
    "collectibles":null,"clan":null,"avatar_decoration_data":null,
    "avatar":"8e1f9d0c2b3a4d5e6f708192a3b4c5d6"},
  "attachments":[],"guild_id":"300000000000000003"}})";

// Reading what Discord sends: the cost of every event the bot hears about.
void BM_Parse(benchmark::State& state) {
  for (auto _ : state) benchmark::DoNotOptimize(Parse(kDispatch));
  state.SetBytesProcessed(state.iterations() *
                          static_cast<int64_t>(kDispatch.size()));
}
BENCHMARK(BM_Parse);

// Picking a few fields out of a parsed dispatch, as the bot does.
void BM_Lookup(benchmark::State& state) {
  const absl::StatusOr<Value> dispatch = Parse(kDispatch);
  for (auto _ : state) {
    benchmark::DoNotOptimize((*dispatch)["d"]["author"]["id"].AsString());
    benchmark::DoNotOptimize((*dispatch)["d"]["content"].AsString());
    benchmark::DoNotOptimize((*dispatch)["s"].AsInt());
  }
}
BENCHMARK(BM_Lookup);

// Writing what the bot sends: here, the body of a message.
void BM_BuildAndSerialize(benchmark::State& state) {
  for (auto _ : state) {
    benchmark::DoNotOptimize(
        Serialize(Value().Set("content",
                              "Good morning <@400000000000000004>! Your "
                              "streak has started! ☀️")));
  }
}
BENCHMARK(BM_BuildAndSerialize);

// Writing a whole document back out.
void BM_Serialize(benchmark::State& state) {
  const absl::StatusOr<Value> dispatch = Parse(kDispatch);
  for (auto _ : state) benchmark::DoNotOptimize(Serialize(*dispatch));
}
BENCHMARK(BM_Serialize);

}  // namespace
}  // namespace json
