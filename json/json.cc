#include "json/json.h"

#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/status_macros.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"

namespace json {
namespace {

// Deep enough for any real document, shallow enough that a hostile one cannot
// overflow the stack.
constexpr int kMaxDepth = 100;

void AppendUtf8(const uint32_t code_point, std::string* const out) {
  if (code_point < 0x80) {
    out->push_back(static_cast<char>(code_point));
  } else if (code_point < 0x800) {
    out->push_back(static_cast<char>(0xC0 | (code_point >> 6)));
    out->push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  } else if (code_point < 0x10000) {
    out->push_back(static_cast<char>(0xE0 | (code_point >> 12)));
    out->push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  } else {
    out->push_back(static_cast<char>(0xF0 | (code_point >> 18)));
    out->push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  }
}

class Parser {
 public:
  explicit Parser(const std::string_view text) : text_(text) {}

  absl::StatusOr<Value> ParseDocument() {
    ABSL_ASSIGN_OR_RETURN(Value value, ParseValue(0));

    SkipWhitespace();
    if (position_ != text_.size()) {
      return Error("unexpected text after the document");
    }

    return value;
  }

 private:
  absl::Status Error(const std::string_view what) const {
    return absl::InvalidArgumentError(
        absl::StrCat("bad JSON at offset ", position_, ": ", what));
  }

  void SkipWhitespace() {
    while (position_ < text_.size()) {
      const char c = text_[position_];
      if (c != ' ' && c != '\n' && c != '\t' && c != '\r') return;
      ++position_;
    }
  }

  // Consumes `expected` if it is next.
  bool Consume(const std::string_view expected) {
    if (!text_.substr(position_).starts_with(expected)) {
      return false;
    }

    position_ += expected.size();
    return true;
  }

  absl::StatusOr<Value> ParseValue(const int depth) {
    if (depth > kMaxDepth) {
      return Error("nested too deeply");
    }

    SkipWhitespace();
    if (position_ == text_.size()) {
      return Error("unexpected end");
    }

    switch (text_[position_]) {
      case '{': return ParseObject(depth);
      case '[': return ParseArray(depth);
      case '"': {
        ABSL_ASSIGN_OR_RETURN(std::string text, ParseString());
        return Value(std::move(text));
      }
      default: break;
    }

    if (Consume("null")) {
      return Value();
    }
    if (Consume("true")) {
      return Value(true);
    }
    if (Consume("false")) {
      return Value(false);
    }

    return ParseNumber();
  }

  absl::StatusOr<Value> ParseObject(const int depth) {
    ++position_;  // The opening brace.
    SkipWhitespace();
    if (Consume("}")) {
      return Value::Object();
    }

    std::vector<std::pair<std::string, Value>> members;
    // Room for an object of ordinary size without growing on the way.
    members.reserve(8);

    while (true) {
      SkipWhitespace();
      if (position_ == text_.size() || text_[position_] != '"') {
        return Error("expected a member name");
      }
      ABSL_ASSIGN_OR_RETURN(std::string key, ParseString());

      SkipWhitespace();
      if (!Consume(":")) {
        return Error("expected ':'");
      }
      ABSL_ASSIGN_OR_RETURN(Value member, ParseValue(depth + 1));
      members.emplace_back(std::move(key), std::move(member));

      SkipWhitespace();
      if (Consume("}")) {
        return Value::Object(std::move(members));
      }
      if (!Consume(",")) {
        return Error("expected ',' or '}'");
      }
    }
  }

  absl::StatusOr<Value> ParseArray(const int depth) {
    std::vector<Value> elements;
    ++position_;  // The opening bracket.
    SkipWhitespace();
    if (Consume("]")) {
      return Value::Array();
    }

    while (true) {
      ABSL_ASSIGN_OR_RETURN(Value element, ParseValue(depth + 1));
      elements.push_back(std::move(element));

      SkipWhitespace();
      if (Consume("]")) {
        return Value::Array(std::move(elements));
      }
      if (!Consume(",")) {
        return Error("expected ',' or ']'");
      }
    }
  }

  absl::StatusOr<uint32_t> ParseHex4() {
    uint32_t unit = 0;
    const std::string_view digits = text_.substr(position_, 4);
    const auto [end, error] =
        std::from_chars(digits.data(), digits.data() + digits.size(), unit, 16);
    if (digits.size() != 4 || error != std::errc() ||
        end != digits.data() + 4) {
      return Error("bad \\u escape");
    }

    position_ += 4;
    return unit;
  }

  absl::StatusOr<std::string> ParseString() {
    std::string out;
    ++position_;  // The opening quote.
    while (true) {
      // Most of most strings is ordinary characters, which are taken a run
      // at a time: up to the next quote, backslash or control character.
      const size_t run = position_;
      while (position_ < text_.size() && text_[position_] != '"' &&
             text_[position_] != '\\' &&
             static_cast<unsigned char>(text_[position_]) >= 0x20) {
        ++position_;
      }
      out.append(text_, run, position_ - run);

      if (position_ == text_.size()) {
        return Error("unterminated string");
      }

      const char c = text_[position_++];
      if (c == '"') {
        return out;
      }
      if (c != '\\') {
        return Error("control character in string");
      }

      // An escape: the character after the backslash says which.
      if (position_ == text_.size()) {
        return Error("unterminated string");
      }
      switch (text_[position_++]) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          ABSL_ASSIGN_OR_RETURN(uint32_t code_point, ParseHex4());
          // Characters outside the basic plane are written as two escapes, a
          // high surrogate then a low one.
          if (code_point >= 0xD800 && code_point < 0xDC00 && Consume("\\u")) {
            ABSL_ASSIGN_OR_RETURN(const uint32_t low, ParseHex4());
            code_point = 0x10000 + ((code_point - 0xD800) << 10) +
                         ((low - 0xDC00) & 0x3FF);
          }
          AppendUtf8(code_point, &out);
          break;
        }
        default: return Error("bad escape");
      }
    }
  }

  absl::StatusOr<Value> ParseNumber() {
    const size_t start = position_;
    while (position_ < text_.size() &&
           absl::StrContains("+-.0123456789eE", text_[position_])) {
      ++position_;
    }
    const std::string_view digits = text_.substr(start, position_ - start);

    int64_t integer = 0;
    if (absl::SimpleAtoi(digits, &integer)) {
      return Value(integer);
    }

    double real = 0;
    if (!digits.empty() && absl::SimpleAtod(digits, &real)) {
      return Value(real);
    }

    position_ = start;
    return Error("expected a value");
  }

  std::string_view text_;
  size_t position_ = 0;
};

void AppendQuoted(const std::string_view text, std::string* const out) {
  out->push_back('"');
  for (const char c : text) {
    switch (c) {
      case '"': out->append("\\\""); break;
      case '\\': out->append("\\\\"); break;
      case '\n': out->append("\\n"); break;
      case '\r': out->append("\\r"); break;
      case '\t': out->append("\\t"); break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          absl::StrAppendFormat(out, "\\u%04x", static_cast<int>(c));
        } else {
          out->push_back(c);
        }
    }
  }
  out->push_back('"');
}

}  // namespace

Value Value::Object() {
  Value value;
  value.data_ = Members();
  return value;
}

Value Value::Array() {
  Value value;
  value.data_ = Elements();
  return value;
}

Value Value::Object(std::vector<std::pair<std::string, Value>> members) {
  Value value;
  value.data_ = std::move(members);
  return value;
}

Value Value::Array(std::vector<Value> elements) {
  Value value;
  value.data_ = std::move(elements);
  return value;
}

Value Value::Take(const std::string_view key) {
  if (Members* const members = std::get_if<Members>(&data_)) {
    for (auto member = members->begin(); member != members->end(); ++member) {
      if (member->first == key) {
        Value taken = std::move(member->second);
        members->erase(member);
        return taken;
      }
    }
  }

  return Value();
}

Value& Value::Set(const std::string_view key, Value value) {
  if (!std::holds_alternative<Members>(data_)) {
    data_ = Members();
  }

  Members& members = std::get<Members>(data_);
  for (auto& [name, member] : members) {
    if (name == key) {
      member = std::move(value);
      return *this;
    }
  }

  members.emplace_back(std::string(key), std::move(value));
  return *this;
}

Value& Value::Append(Value value) {
  if (!std::holds_alternative<Elements>(data_)) {
    data_ = Elements();
  }

  std::get<Elements>(data_).push_back(std::move(value));
  return *this;
}

bool Value::AsBool() const {
  const bool* const value = std::get_if<bool>(&data_);
  return value != nullptr && *value;
}

int64_t Value::AsInt() const {
  if (const int64_t* const integer = std::get_if<int64_t>(&data_)) {
    return *integer;
  }
  if (const double* const real = std::get_if<double>(&data_)) {
    return static_cast<int64_t>(*real);
  }
  return 0;
}

double Value::AsDouble() const {
  if (const double* const real = std::get_if<double>(&data_)) {
    return *real;
  }
  if (const int64_t* const integer = std::get_if<int64_t>(&data_)) {
    return static_cast<double>(*integer);
  }
  return 0;
}

const std::string& Value::AsString() const {
  static const std::string* const empty = new std::string();
  const std::string* const value = std::get_if<std::string>(&data_);
  return value != nullptr ? *value : *empty;
}

const Value& Value::operator[](const std::string_view key) const {
  static const Value* const null = new Value();
  if (const Members* const members = std::get_if<Members>(&data_)) {
    for (const auto& [name, member] : *members) {
      if (name == key) {
        return member;
      }
    }
  }
  return *null;
}

std::span<const Value> Value::items() const {
  const Elements* const elements = std::get_if<Elements>(&data_);
  return elements != nullptr ? std::span<const Value>(*elements)
                             : std::span<const Value>();
}

absl::StatusOr<Value> Parse(const std::string_view text) {
  return Parser(text).ParseDocument();
}

std::string Serialize(const Value& value) {
  struct Writer {
    std::string out;

    void operator()(std::monostate) { out.append("null"); }
    void operator()(const bool b) { out.append(b ? "true" : "false"); }
    void operator()(const int64_t n) { absl::StrAppend(&out, n); }
    void operator()(const double d) {
      if (!std::isfinite(d)) {
        out.append("null");  // JSON has no way to write these.
        return;
      }

      // The shortest text that reads back as the same number.
      std::array<char, 32> text{};
      const auto [end, error] =
          std::to_chars(text.data(), text.data() + text.size(), d);
      out.append(text.data(), end);
    }
    void operator()(const std::string& s) { AppendQuoted(s, &out); }

    void operator()(const Value::Elements& elements) {
      out.push_back('[');
      for (const Value& element : elements) {
        if (&element != elements.data()) {
          out.push_back(',');
        }
        std::visit(*this, element.data_);
      }
      out.push_back(']');
    }

    void operator()(const Value::Members& members) {
      out.push_back('{');
      for (const auto& member : members) {
        if (&member != members.data()) {
          out.push_back(',');
        }
        AppendQuoted(member.first, &out);
        out.push_back(':');
        std::visit(*this, member.second.data_);
      }
      out.push_back('}');
    }
  };

  Writer writer;
  std::visit(writer, value.data_);
  return std::move(writer.out);
}

}  // namespace json
