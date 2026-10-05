// JSON documents: parse text into a `Value`, read it, build one, and turn it
// back into text.
//
// Reading a Value cannot fail. Asking for a member that is missing, or for a
// value as a type it is not, yields null, zero, false or an empty string, so
// code that picks a few fields out of a large document needs no error
// handling beyond checking the fields it cannot do without.

#ifndef JSON_JSON_H_
#define JSON_JSON_H_

#include <concepts>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "absl/status/statusor.h"

namespace json {

class Value {
 public:
  // Null.
  Value() = default;

  // Scalars convert implicitly, so they can be passed straight to Set and
  // Append.
  Value(bool value) : data_(value) {}  // NOLINT(google-explicit-constructor)
  template <std::integral T>
  Value(T value)  // NOLINT(google-explicit-constructor)
      : data_(static_cast<int64_t>(value)) {}
  Value(double value) : data_(value) {}  // NOLINT(google-explicit-constructor)
  Value(std::string value)               // NOLINT(google-explicit-constructor)
      : data_(std::move(value)) {}
  Value(std::string_view value)  // NOLINT(google-explicit-constructor)
      : data_(std::string(value)) {}
  Value(const char* value)  // NOLINT(google-explicit-constructor)
      : data_(std::string(value)) {}

  // An object with no members, and an array with no elements.
  static Value Object();
  static Value Array();

  // An object with these members and an array of these elements, in the
  // order given: for whoever already has them all in hand, which is quicker
  // than a Set or an Append for each. If two members have the same name,
  // looking it up finds the first.
  static Value Object(std::vector<std::pair<std::string, Value>> members);
  static Value Array(std::vector<Value> elements);

  // Removes member `key` of an object and returns it: the way to take part
  // of a document without copying it. Null if there is none, or this is not
  // an object.
  Value Take(std::string_view key);

  // Sets member `key`, replacing any existing one. A value that is not yet an
  // object becomes an empty one first. Returns *this, for chaining.
  Value& Set(std::string_view key, Value value);

  // Adds an element at the end. A value that is not yet an array becomes an
  // empty one first. Returns *this, for chaining.
  Value& Append(Value value);

  bool is_null() const { return std::holds_alternative<std::monostate>(data_); }

  // The value as the type asked for, or that type's zero if it is another
  // type. Numbers convert between integer and floating point.
  bool AsBool() const;
  int64_t AsInt() const;
  double AsDouble() const;
  const std::string& AsString() const;

  // Member `key` of an object. Null if there is none or this is not an object.
  const Value& operator[](std::string_view key) const;

  // The elements of an array. Empty if this is not an array.
  std::span<const Value> items() const;

  friend bool operator==(const Value&, const Value&) = default;

 private:
  friend std::string Serialize(const Value& value);

  using Elements = std::vector<Value>;
  // In insertion order, which is also the order members are serialized in.
  using Members = std::vector<std::pair<std::string, Value>>;

  std::variant<std::monostate, bool, int64_t, double, std::string, Elements,
               Members>
      data_;
};

// Parses a complete JSON document. Fails with kInvalidArgument, saying where,
// if `text` is not one. Should an object name a member twice, which JSON
// allows and advises against, looking the name up finds the first.
absl::StatusOr<Value> Parse(std::string_view text);

// The compact JSON text for `value`. Strings are taken to be UTF-8.
std::string Serialize(const Value& value);

}  // namespace json

#endif  // JSON_JSON_H_
