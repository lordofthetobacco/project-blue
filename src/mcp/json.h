#ifndef BLUE_MCP_JSON_H_
#define BLUE_MCP_JSON_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"

namespace blue::mcp {

struct JsonMember;

// A JSON value (RFC 8259): null, boolean, number, string, array or object.
// Used for every MCP message and for the arguments and results of tools.
//
// Numbers keep their integer form when they were written without a fraction
// or exponent and fit int64_t (ids, counts), so they round-trip exactly;
// every number is also readable as a double. Objects keep insertion order and
// are searched linearly, which suits the small objects MCP exchanges.
//
// Building:
//   Json result = Json::Object();
//   result.Set("name", "crate").Set("count", 3).Set("visible", true);
//   Json list = Json::Array();
//   list.Push(Json::Number(1.5));
//   result.Set("values", std::move(list));
//
// Reading:
//   if (const std::string* name = args.GetString("name")) { ... }
//   std::optional<double> yaw = args.GetNumber("yaw");
//
// A value type: copyable and movable. Not thread-safe for concurrent mutation;
// const operations may run concurrently.
class Json {
 public:
  enum class Type { kNull, kBool, kNumber, kString, kArray, kObject };

  // Returns a null value (same as the default constructor).
  static Json Null() { return Json(); }
  static Json Bool(bool value);
  static Json Number(double value);
  static Json Integer(int64_t value);
  static Json String(std::string value);
  // Returns an empty array / object.
  static Json Array();
  static Json Object();

  Json();
  Json(const Json&);
  Json& operator=(const Json&);
  Json(Json&&) noexcept;
  Json& operator=(Json&&) noexcept;
  ~Json();

  Type type() const { return type_; }
  bool is_null() const { return type_ == Type::kNull; }
  bool is_bool() const { return type_ == Type::kBool; }
  bool is_number() const { return type_ == Type::kNumber; }
  // True for numbers held in integer form (see the class comment).
  bool is_integer() const { return type_ == Type::kNumber && is_integer_; }
  bool is_string() const { return type_ == Type::kString; }
  bool is_array() const { return type_ == Type::kArray; }
  bool is_object() const { return type_ == Type::kObject; }

  // Typed accessors. Each returns a neutral value (false, 0, empty) when the
  // value has another type, so callers check the type first when it matters.
  bool bool_value() const;
  double number_value() const;
  // Integer value; a non-integral number is truncated toward zero.
  int64_t integer_value() const;
  const std::string& string_value() const;
  const std::vector<Json>& array_items() const;
  const std::vector<JsonMember>& object_items() const;

  // Number of array items or object members; 0 for other types.
  size_t size() const;

  // Returns the member named `key`, or nullptr when absent or when this is
  // not an object. The pointer is valid until this object is modified.
  const Json* Find(std::string_view key) const;
  Json* Find(std::string_view key);

  // Typed member lookups: nullopt / nullptr when the member is missing or
  // has another type. GetInteger accepts any number that is integral.
  std::optional<bool> GetBool(std::string_view key) const;
  std::optional<double> GetNumber(std::string_view key) const;
  std::optional<int64_t> GetInteger(std::string_view key) const;
  const std::string* GetString(std::string_view key) const;
  const Json* GetArray(std::string_view key) const;
  const Json* GetObject(std::string_view key) const;

  // Sets member `key`, replacing an existing member of that name. Turns a
  // null value into an object first; fatal on other non-object types.
  // Returns *this for chaining. Integers other than int and int64_t must be
  // cast by the caller (the overload set would be ambiguous).
  Json& Set(std::string_view key, Json value);
  Json& Set(std::string_view key, bool value);
  Json& Set(std::string_view key, int value);
  Json& Set(std::string_view key, int64_t value);
  Json& Set(std::string_view key, double value);
  Json& Set(std::string_view key, std::string_view value);
  Json& Set(std::string_view key, const char* value);

  // Appends to an array. Turns a null value into an array first; fatal on
  // other non-array types. Returns *this for chaining.
  Json& Push(Json value);

  // Removes member `key`; returns whether it existed.
  bool Erase(std::string_view key);

  // Returns compact JSON text (no whitespace). Non-finite numbers are written
  // as null; strings are escaped as required, non-ASCII UTF-8 is kept as is.
  std::string Dump() const;
  // Returns indented JSON text (two spaces), easier for people and models to
  // read in tool results.
  std::string DumpPretty() const;

 private:
  void DumpTo(std::string& out, int indent, int depth) const;

  Type type_ = Type::kNull;
  bool bool_ = false;
  bool is_integer_ = false;
  int64_t integer_ = 0;
  double number_ = 0.0;
  std::string string_;
  std::vector<Json> array_;
  std::vector<JsonMember> object_;
};

// One object member.
struct JsonMember {
  std::string key;
  Json value;
};

// Structural equality: same type and contents. Integer and floating forms of
// the same number compare equal; object member order matters.
bool operator==(const Json& a, const Json& b);

// Describes why ParseJson rejected its input.
struct JsonError {
  std::string message;
  // Byte offset into the input where the problem was found.
  size_t offset = 0;
};

// Parses exactly one JSON value from `text` (surrounding whitespace allowed).
// Rejects trailing data, invalid UTF-8 in strings, unpaired surrogates and
// nesting deeper than 128 levels. Duplicate object keys keep the last value.
core::Result<Json, JsonError> ParseJson(std::string_view text);

// Returns standard base64 (RFC 4648, with padding) of `bytes`, for MCP image
// and blob content.
std::string EncodeBase64(const void* bytes, size_t size);

}  // namespace blue::mcp

#endif  // BLUE_MCP_JSON_H_
