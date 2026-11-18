#include "mcp/json.h"

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "core/log.h"
#include "core/result.h"

namespace blue::mcp {

namespace {

constexpr int kMaxDepth = 128;

const std::string& EmptyString() {
  // Never destroyed: function-local static with a nontrivial destructor is
  // not allowed, so the empty values are leaked deliberately.
  static const std::string* const empty = new std::string();
  return *empty;
}

const std::vector<Json>& EmptyArray() {
  static const std::vector<Json>* const empty = new std::vector<Json>();
  return *empty;
}

const std::vector<JsonMember>& EmptyObject() {
  static const std::vector<JsonMember>* const empty =
      new std::vector<JsonMember>();
  return *empty;
}

// Appends `text` as a quoted JSON string.
void AppendQuoted(std::string& out, std::string_view text) {
  constexpr char kHex[] = "0123456789abcdef";
  out.push_back('"');
  for (char c : text) {
    const auto byte = static_cast<unsigned char>(c);
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      default:
        if (byte < 0x20) {
          out += "\\u00";
          out.push_back(kHex[byte >> 4]);
          out.push_back(kHex[byte & 0xF]);
        } else {
          out.push_back(c);
        }
        break;
    }
  }
  out.push_back('"');
}

void AppendDouble(std::string& out, double value) {
  if (!std::isfinite(value)) {
    out += "null";
    return;
  }
  char buffer[64];
  const std::to_chars_result result =
      std::to_chars(buffer, buffer + sizeof(buffer), value);
  out.append(buffer, result.ptr);
}

void AppendInteger(std::string& out, int64_t value) {
  char buffer[32];
  const std::to_chars_result result =
      std::to_chars(buffer, buffer + sizeof(buffer), value);
  out.append(buffer, result.ptr);
}

void AppendNewline(std::string& out, int indent, int depth) {
  if (indent < 0) {
    return;
  }
  out.push_back('\n');
  out.append(static_cast<size_t>(indent) * static_cast<size_t>(depth), ' ');
}

// Recursive-descent parser over a string_view. Stops at the first error.
class Parser {
 public:
  explicit Parser(std::string_view text) : text_(text) {}

  core::Result<Json, JsonError> ParseDocument() {
    SkipWhitespace();
    std::optional<Json> value = ParseValue(0);
    if (!value) {
      return core::Result<Json, JsonError>::Error(std::move(error_));
    }
    SkipWhitespace();
    if (pos_ != text_.size()) {
      Fail("unexpected data after the JSON value");
      return core::Result<Json, JsonError>::Error(std::move(error_));
    }
    return core::Result<Json, JsonError>::Ok(std::move(*value));
  }

 private:
  void Fail(std::string message) {
    if (!failed_) {
      failed_ = true;
      error_ = JsonError{std::move(message), pos_};
    }
  }

  bool AtEnd() const { return pos_ >= text_.size(); }
  char Peek() const { return AtEnd() ? '\0' : text_[pos_]; }

  void SkipWhitespace() {
    while (!AtEnd()) {
      const char c = text_[pos_];
      if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
        return;
      }
      ++pos_;
    }
  }

  bool ConsumeLiteral(std::string_view literal) {
    if (text_.substr(pos_, literal.size()) != literal) {
      Fail("invalid literal");
      return false;
    }
    pos_ += literal.size();
    return true;
  }

  std::optional<Json> ParseValue(int depth) {
    if (depth > kMaxDepth) {
      Fail("nesting too deep");
      return std::nullopt;
    }
    switch (Peek()) {
      case '{':
        return ParseObject(depth);
      case '[':
        return ParseArray(depth);
      case '"': {
        std::optional<std::string> text = ParseString();
        if (!text) {
          return std::nullopt;
        }
        return Json::String(std::move(*text));
      }
      case 't':
        return ConsumeLiteral("true") ? std::optional<Json>(Json::Bool(true))
                                      : std::nullopt;
      case 'f':
        return ConsumeLiteral("false") ? std::optional<Json>(Json::Bool(false))
                                       : std::nullopt;
      case 'n':
        return ConsumeLiteral("null") ? std::optional<Json>(Json::Null())
                                      : std::nullopt;
      default:
        if (Peek() == '-' || (Peek() >= '0' && Peek() <= '9')) {
          return ParseNumber();
        }
        Fail(AtEnd() ? "unexpected end of input" : "unexpected character");
        return std::nullopt;
    }
  }

  std::optional<Json> ParseObject(int depth) {
    ++pos_;  // '{'
    Json object = Json::Object();
    SkipWhitespace();
    if (Peek() == '}') {
      ++pos_;
      return object;
    }
    while (true) {
      SkipWhitespace();
      if (Peek() != '"') {
        Fail("expected a member name");
        return std::nullopt;
      }
      std::optional<std::string> key = ParseString();
      if (!key) {
        return std::nullopt;
      }
      SkipWhitespace();
      if (Peek() != ':') {
        Fail("expected ':'");
        return std::nullopt;
      }
      ++pos_;
      SkipWhitespace();
      std::optional<Json> value = ParseValue(depth + 1);
      if (!value) {
        return std::nullopt;
      }
      object.Set(*key, std::move(*value));
      SkipWhitespace();
      if (Peek() == ',') {
        ++pos_;
        continue;
      }
      if (Peek() == '}') {
        ++pos_;
        return object;
      }
      Fail("expected ',' or '}'");
      return std::nullopt;
    }
  }

  std::optional<Json> ParseArray(int depth) {
    ++pos_;  // '['
    Json array = Json::Array();
    SkipWhitespace();
    if (Peek() == ']') {
      ++pos_;
      return array;
    }
    while (true) {
      SkipWhitespace();
      std::optional<Json> value = ParseValue(depth + 1);
      if (!value) {
        return std::nullopt;
      }
      array.Push(std::move(*value));
      SkipWhitespace();
      if (Peek() == ',') {
        ++pos_;
        continue;
      }
      if (Peek() == ']') {
        ++pos_;
        return array;
      }
      Fail("expected ',' or ']'");
      return std::nullopt;
    }
  }

  std::optional<Json> ParseNumber() {
    const size_t start = pos_;
    if (Peek() == '-') {
      ++pos_;
    }
    if (Peek() == '0') {
      ++pos_;
    } else if (Peek() >= '1' && Peek() <= '9') {
      SkipDigits();
    } else {
      Fail("invalid number");
      return std::nullopt;
    }
    bool integral = true;
    if (Peek() == '.') {
      integral = false;
      ++pos_;
      if (!(Peek() >= '0' && Peek() <= '9')) {
        Fail("invalid number");
        return std::nullopt;
      }
      SkipDigits();
    }
    if (Peek() == 'e' || Peek() == 'E') {
      integral = false;
      ++pos_;
      if (Peek() == '+' || Peek() == '-') {
        ++pos_;
      }
      if (!(Peek() >= '0' && Peek() <= '9')) {
        Fail("invalid number");
        return std::nullopt;
      }
      SkipDigits();
    }
    const char* first = text_.data() + start;
    const char* last = text_.data() + pos_;
    if (integral) {
      int64_t value = 0;
      const std::from_chars_result result = std::from_chars(first, last, value);
      if (result.ec == std::errc() && result.ptr == last) {
        return Json::Integer(value);
      }
    }
    double value = 0.0;
    const std::from_chars_result result = std::from_chars(first, last, value);
    if (result.ptr != last || (result.ec != std::errc() &&
                               result.ec != std::errc::result_out_of_range)) {
      Fail("invalid number");
      return std::nullopt;
    }
    return Json::Number(value);
  }

  void SkipDigits() {
    while (Peek() >= '0' && Peek() <= '9') {
      ++pos_;
    }
  }

  // Reads 4 hex digits; -1 on error.
  int ReadHex4() {
    if (text_.size() - pos_ < 4) {
      return -1;
    }
    int value = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = text_[pos_ + static_cast<size_t>(i)];
      int digit = 0;
      if (c >= '0' && c <= '9') {
        digit = c - '0';
      } else if (c >= 'a' && c <= 'f') {
        digit = c - 'a' + 10;
      } else if (c >= 'A' && c <= 'F') {
        digit = c - 'A' + 10;
      } else {
        return -1;
      }
      value = value * 16 + digit;
    }
    pos_ += 4;
    return value;
  }

  static void AppendUtf8(std::string& out, uint32_t code_point) {
    if (code_point < 0x80) {
      out.push_back(static_cast<char>(code_point));
    } else if (code_point < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
      out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else if (code_point < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
      out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
      out.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    }
  }

  // Parses the \u escape after "\u" (pos_ at the first hex digit).
  bool ParseUnicodeEscape(std::string& out) {
    const int high = ReadHex4();
    if (high < 0) {
      Fail("invalid \\u escape");
      return false;
    }
    uint32_t code_point = static_cast<uint32_t>(high);
    if (high >= 0xD800 && high <= 0xDBFF) {
      if (text_.substr(pos_, 2) != "\\u") {
        Fail("unpaired surrogate");
        return false;
      }
      pos_ += 2;
      const int low = ReadHex4();
      if (low < 0xDC00 || low > 0xDFFF) {
        Fail("unpaired surrogate");
        return false;
      }
      code_point = 0x10000 + ((static_cast<uint32_t>(high) - 0xD800) << 10) +
                   (static_cast<uint32_t>(low) - 0xDC00);
    } else if (high >= 0xDC00 && high <= 0xDFFF) {
      Fail("unpaired surrogate");
      return false;
    }
    AppendUtf8(out, code_point);
    return true;
  }

  // Validates one UTF-8 sequence starting at pos_ and copies it to `out`.
  bool CopyUtf8Sequence(std::string& out) {
    const auto lead = static_cast<unsigned char>(text_[pos_]);
    int length = 0;
    uint32_t code_point = 0;
    if (lead >= 0xC2 && lead <= 0xDF) {
      length = 2;
      code_point = lead & 0x1Fu;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
      length = 3;
      code_point = lead & 0x0Fu;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
      length = 4;
      code_point = lead & 0x07u;
    } else {
      Fail("invalid UTF-8");
      return false;
    }
    if (text_.size() - pos_ < static_cast<size_t>(length)) {
      Fail("invalid UTF-8");
      return false;
    }
    for (int i = 1; i < length; ++i) {
      const auto byte =
          static_cast<unsigned char>(text_[pos_ + static_cast<size_t>(i)]);
      if ((byte & 0xC0) != 0x80) {
        Fail("invalid UTF-8");
        return false;
      }
      code_point = (code_point << 6) | (byte & 0x3Fu);
    }
    const bool overlong = (length == 3 && code_point < 0x800) ||
                          (length == 4 && code_point < 0x10000);
    const bool surrogate = code_point >= 0xD800 && code_point <= 0xDFFF;
    if (overlong || surrogate || code_point > 0x10FFFF) {
      Fail("invalid UTF-8");
      return false;
    }
    out.append(text_.substr(pos_, static_cast<size_t>(length)));
    pos_ += static_cast<size_t>(length);
    return true;
  }

  std::optional<std::string> ParseString() {
    ++pos_;  // opening quote
    std::string out;
    while (true) {
      if (AtEnd()) {
        Fail("unterminated string");
        return std::nullopt;
      }
      const char c = text_[pos_];
      const auto byte = static_cast<unsigned char>(c);
      if (c == '"') {
        ++pos_;
        return out;
      }
      if (byte < 0x20) {
        Fail("control character in string");
        return std::nullopt;
      }
      if (byte >= 0x80) {
        if (!CopyUtf8Sequence(out)) {
          return std::nullopt;
        }
        continue;
      }
      ++pos_;
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      if (AtEnd()) {
        Fail("unterminated string");
        return std::nullopt;
      }
      const char escape = text_[pos_++];
      switch (escape) {
        case '"':
          out.push_back('"');
          break;
        case '\\':
          out.push_back('\\');
          break;
        case '/':
          out.push_back('/');
          break;
        case 'b':
          out.push_back('\b');
          break;
        case 'f':
          out.push_back('\f');
          break;
        case 'n':
          out.push_back('\n');
          break;
        case 'r':
          out.push_back('\r');
          break;
        case 't':
          out.push_back('\t');
          break;
        case 'u':
          if (!ParseUnicodeEscape(out)) {
            return std::nullopt;
          }
          break;
        default:
          Fail("invalid escape");
          return std::nullopt;
      }
    }
  }

  std::string_view text_;
  size_t pos_ = 0;
  bool failed_ = false;
  JsonError error_;
};

}  // namespace

Json Json::Bool(bool value) {
  Json json;
  json.type_ = Type::kBool;
  json.bool_ = value;
  return json;
}

Json Json::Number(double value) {
  Json json;
  json.type_ = Type::kNumber;
  json.number_ = value;
  json.integer_ = 0;
  json.is_integer_ = false;
  return json;
}

Json Json::Integer(int64_t value) {
  Json json;
  json.type_ = Type::kNumber;
  json.integer_ = value;
  json.number_ = static_cast<double>(value);
  json.is_integer_ = true;
  return json;
}

Json Json::String(std::string value) {
  Json json;
  json.type_ = Type::kString;
  json.string_ = std::move(value);
  return json;
}

Json Json::Array() {
  Json json;
  json.type_ = Type::kArray;
  return json;
}

Json Json::Object() {
  Json json;
  json.type_ = Type::kObject;
  return json;
}

Json::Json() = default;
Json::Json(const Json&) = default;
Json& Json::operator=(const Json&) = default;
Json::Json(Json&&) noexcept = default;
Json& Json::operator=(Json&&) noexcept = default;
Json::~Json() = default;

bool Json::bool_value() const { return type_ == Type::kBool && bool_; }

double Json::number_value() const {
  return type_ == Type::kNumber ? number_ : 0.0;
}

int64_t Json::integer_value() const {
  if (type_ != Type::kNumber) {
    return 0;
  }
  if (is_integer_) {
    return integer_;
  }
  // Clamp to the representable range before converting.
  if (!(number_ > -9.2e18 && number_ < 9.2e18)) {
    return 0;
  }
  return static_cast<int64_t>(number_);
}

const std::string& Json::string_value() const {
  return type_ == Type::kString ? string_ : EmptyString();
}

const std::vector<Json>& Json::array_items() const {
  return type_ == Type::kArray ? array_ : EmptyArray();
}

const std::vector<JsonMember>& Json::object_items() const {
  return type_ == Type::kObject ? object_ : EmptyObject();
}

size_t Json::size() const {
  if (type_ == Type::kArray) {
    return array_.size();
  }
  if (type_ == Type::kObject) {
    return object_.size();
  }
  return 0;
}

const Json* Json::Find(std::string_view key) const {
  if (type_ != Type::kObject) {
    return nullptr;
  }
  for (const JsonMember& member : object_) {
    if (member.key == key) {
      return &member.value;
    }
  }
  return nullptr;
}

Json* Json::Find(std::string_view key) {
  return const_cast<Json*>(static_cast<const Json*>(this)->Find(key));
}

std::optional<bool> Json::GetBool(std::string_view key) const {
  const Json* value = Find(key);
  if (value == nullptr || !value->is_bool()) {
    return std::nullopt;
  }
  return value->bool_value();
}

std::optional<double> Json::GetNumber(std::string_view key) const {
  const Json* value = Find(key);
  if (value == nullptr || !value->is_number()) {
    return std::nullopt;
  }
  return value->number_value();
}

std::optional<int64_t> Json::GetInteger(std::string_view key) const {
  const Json* value = Find(key);
  if (value == nullptr || !value->is_number()) {
    return std::nullopt;
  }
  if (value->is_integer()) {
    return value->integer_value();
  }
  const double number = value->number_value();
  if (std::trunc(number) != number || !(number > -9.2e18 && number < 9.2e18)) {
    return std::nullopt;
  }
  return static_cast<int64_t>(number);
}

const std::string* Json::GetString(std::string_view key) const {
  const Json* value = Find(key);
  return value != nullptr && value->is_string() ? &value->string_ : nullptr;
}

const Json* Json::GetArray(std::string_view key) const {
  const Json* value = Find(key);
  return value != nullptr && value->is_array() ? value : nullptr;
}

const Json* Json::GetObject(std::string_view key) const {
  const Json* value = Find(key);
  return value != nullptr && value->is_object() ? value : nullptr;
}

Json& Json::Set(std::string_view key, Json value) {
  if (type_ == Type::kNull) {
    type_ = Type::kObject;
  }
  if (type_ != Type::kObject) {
    core::log::Fatal("Json::Set on a non-object value");
  }
  if (Json* existing = Find(key)) {
    *existing = std::move(value);
  } else {
    object_.push_back(JsonMember{std::string(key), std::move(value)});
  }
  return *this;
}

Json& Json::Set(std::string_view key, bool value) {
  return Set(key, Json::Bool(value));
}

Json& Json::Set(std::string_view key, int value) {
  return Set(key, Json::Integer(value));
}

Json& Json::Set(std::string_view key, int64_t value) {
  return Set(key, Json::Integer(value));
}

Json& Json::Set(std::string_view key, double value) {
  return Set(key, Json::Number(value));
}

Json& Json::Set(std::string_view key, std::string_view value) {
  return Set(key, Json::String(std::string(value)));
}

Json& Json::Set(std::string_view key, const char* value) {
  return Set(key, Json::String(std::string(value)));
}

Json& Json::Push(Json value) {
  if (type_ == Type::kNull) {
    type_ = Type::kArray;
  }
  if (type_ != Type::kArray) {
    core::log::Fatal("Json::Push on a non-array value");
  }
  array_.push_back(std::move(value));
  return *this;
}

bool Json::Erase(std::string_view key) {
  if (type_ != Type::kObject) {
    return false;
  }
  for (auto it = object_.begin(); it != object_.end(); ++it) {
    if (it->key == key) {
      object_.erase(it);
      return true;
    }
  }
  return false;
}

std::string Json::Dump() const {
  std::string out;
  DumpTo(out, -1, 0);
  return out;
}

std::string Json::DumpPretty() const {
  std::string out;
  DumpTo(out, 2, 0);
  return out;
}

void Json::DumpTo(std::string& out, int indent, int depth) const {
  switch (type_) {
    case Type::kNull:
      out += "null";
      return;
    case Type::kBool:
      out += bool_ ? "true" : "false";
      return;
    case Type::kNumber:
      if (is_integer_) {
        AppendInteger(out, integer_);
      } else {
        AppendDouble(out, number_);
      }
      return;
    case Type::kString:
      AppendQuoted(out, string_);
      return;
    case Type::kArray: {
      out.push_back('[');
      bool first = true;
      for (const Json& item : array_) {
        if (!first) {
          out.push_back(',');
        }
        first = false;
        AppendNewline(out, indent, depth + 1);
        item.DumpTo(out, indent, depth + 1);
      }
      if (!array_.empty()) {
        AppendNewline(out, indent, depth);
      }
      out.push_back(']');
      return;
    }
    case Type::kObject: {
      out.push_back('{');
      bool first = true;
      for (const JsonMember& member : object_) {
        if (!first) {
          out.push_back(',');
        }
        first = false;
        AppendNewline(out, indent, depth + 1);
        AppendQuoted(out, member.key);
        out += indent < 0 ? ":" : ": ";
        member.value.DumpTo(out, indent, depth + 1);
      }
      if (!object_.empty()) {
        AppendNewline(out, indent, depth);
      }
      out.push_back('}');
      return;
    }
  }
}

bool operator==(const Json& a, const Json& b) {
  if (a.type() != b.type()) {
    return false;
  }
  switch (a.type()) {
    case Json::Type::kNull:
      return true;
    case Json::Type::kBool:
      return a.bool_value() == b.bool_value();
    case Json::Type::kNumber:
      if (a.is_integer() && b.is_integer()) {
        return a.integer_value() == b.integer_value();
      }
      return a.number_value() == b.number_value();
    case Json::Type::kString:
      return a.string_value() == b.string_value();
    case Json::Type::kArray:
      return a.array_items() == b.array_items();
    case Json::Type::kObject: {
      const std::vector<JsonMember>& left = a.object_items();
      const std::vector<JsonMember>& right = b.object_items();
      if (left.size() != right.size()) {
        return false;
      }
      for (size_t i = 0; i < left.size(); ++i) {
        if (left[i].key != right[i].key || !(left[i].value == right[i].value)) {
          return false;
        }
      }
      return true;
    }
  }
  return false;
}

core::Result<Json, JsonError> ParseJson(std::string_view text) {
  return Parser(text).ParseDocument();
}

std::string EncodeBase64(const void* bytes, size_t size) {
  constexpr char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  const auto* data = static_cast<const unsigned char*>(bytes);
  std::string out;
  out.reserve((size + 2) / 3 * 4);
  size_t i = 0;
  for (; i + 3 <= size; i += 3) {
    const uint32_t triple =
        (uint32_t{data[i]} << 16) | (uint32_t{data[i + 1]} << 8) | data[i + 2];
    out.push_back(kAlphabet[(triple >> 18) & 0x3F]);
    out.push_back(kAlphabet[(triple >> 12) & 0x3F]);
    out.push_back(kAlphabet[(triple >> 6) & 0x3F]);
    out.push_back(kAlphabet[triple & 0x3F]);
  }
  const size_t rest = size - i;
  if (rest == 1) {
    const uint32_t triple = uint32_t{data[i]} << 16;
    out.push_back(kAlphabet[(triple >> 18) & 0x3F]);
    out.push_back(kAlphabet[(triple >> 12) & 0x3F]);
    out += "==";
  } else if (rest == 2) {
    const uint32_t triple =
        (uint32_t{data[i]} << 16) | (uint32_t{data[i + 1]} << 8);
    out.push_back(kAlphabet[(triple >> 18) & 0x3F]);
    out.push_back(kAlphabet[(triple >> 12) & 0x3F]);
    out.push_back(kAlphabet[(triple >> 6) & 0x3F]);
    out += '=';
  }
  return out;
}

}  // namespace blue::mcp
