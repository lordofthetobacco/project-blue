#include "mcp/tool.h"

#include <cmath>
#include <optional>
#include <string>
#include <utility>

#include "mcp/json.h"

namespace blue::mcp {

namespace {

// Returns whether `value` matches the JSON Schema type name `type`. Unknown
// type names are accepted (the subset does not check them).
bool MatchesType(const Json& value, const std::string& type) {
  if (type == "string") {
    return value.is_string();
  }
  if (type == "number") {
    return value.is_number();
  }
  if (type == "integer") {
    return value.is_number() &&
           std::trunc(value.number_value()) == value.number_value();
  }
  if (type == "boolean") {
    return value.is_bool();
  }
  if (type == "object") {
    return value.is_object();
  }
  if (type == "array") {
    return value.is_array();
  }
  if (type == "null") {
    return value.is_null();
  }
  return true;
}

// Checks `value` against the "type" (a name or an array of names) and
// "enum" keywords of `schema`.
std::optional<std::string> CheckTypeAndEnum(const Json& schema,
                                            const Json& value,
                                            const std::string& path) {
  if (const Json* type = schema.Find("type")) {
    bool matches = false;
    std::string expected;
    if (type->is_string()) {
      matches = MatchesType(value, type->string_value());
      expected = type->string_value();
    } else if (type->is_array()) {
      for (const Json& name : type->array_items()) {
        matches = matches || MatchesType(value, name.string_value());
        expected += expected.empty() ? "" : " or ";
        expected += name.string_value();
      }
    } else {
      matches = true;
    }
    if (!matches) {
      return path + " must be " + expected;
    }
  }
  if (const Json* allowed = schema.GetArray("enum")) {
    for (const Json& option : allowed->array_items()) {
      if (option == value) {
        return std::nullopt;
      }
    }
    return path + " must be one of " + allowed->Dump();
  }
  return std::nullopt;
}

std::optional<std::string> Validate(const Json& schema, const Json& value,
                                    const std::string& path, int depth) {
  if (!schema.is_object()) {
    return std::nullopt;
  }
  if (std::optional<std::string> error =
          CheckTypeAndEnum(schema, value, path)) {
    return error;
  }
  if (depth >= 3) {
    return std::nullopt;
  }
  if (value.is_object()) {
    if (const Json* required = schema.GetArray("required")) {
      for (const Json& name : required->array_items()) {
        if (name.is_string() && value.Find(name.string_value()) == nullptr) {
          return "missing required argument " + path + "." +
                 name.string_value();
        }
      }
    }
    if (const Json* properties = schema.GetObject("properties")) {
      for (const JsonMember& property : properties->object_items()) {
        const Json* member = value.Find(property.key);
        if (member == nullptr) {
          continue;
        }
        if (std::optional<std::string> error =
                Validate(property.value, *member, path + "." + property.key,
                         depth + 1)) {
          return error;
        }
      }
    }
  }
  if (value.is_array()) {
    if (const Json* items = schema.GetObject("items")) {
      for (size_t i = 0; i < value.size(); ++i) {
        if (std::optional<std::string> error =
                Validate(*items, value.array_items()[i],
                         path + "[" + std::to_string(i) + "]", depth + 1)) {
          return error;
        }
      }
    }
  }
  return std::nullopt;
}

}  // namespace

ToolResult ToolResult::Text(std::string text) {
  ToolResult result;
  result.AddText(std::move(text));
  return result;
}

ToolResult ToolResult::Error(std::string message) {
  ToolResult result = Text(std::move(message));
  result.is_error = true;
  return result;
}

ToolResult ToolResult::Structured(Json value) {
  ToolResult result = Text(value.DumpPretty());
  result.structured = std::move(value);
  return result;
}

ToolResult& ToolResult::AddImage(std::string base64_data,
                                 std::string mime_type) {
  ContentItem item;
  item.kind = ContentItem::Kind::kImage;
  item.data = std::move(base64_data);
  item.mime_type = std::move(mime_type);
  content.push_back(std::move(item));
  return *this;
}

ToolResult& ToolResult::AddText(std::string text) {
  ContentItem item;
  item.kind = ContentItem::Kind::kText;
  item.text = std::move(text);
  content.push_back(std::move(item));
  return *this;
}

std::optional<std::string> ValidateAgainstSchema(const Json& schema,
                                                 const Json& arguments) {
  return Validate(schema, arguments, "arguments", 0);
}

}  // namespace blue::mcp
