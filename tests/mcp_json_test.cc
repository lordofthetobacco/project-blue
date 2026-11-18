#include <doctest/doctest.h>

#include <cstdint>
#include <string>

#include "mcp/json.h"
#include "mcp/tool.h"

namespace blue::mcp {
namespace {

Json Parse(const std::string& text) {
  auto parsed = ParseJson(text);
  REQUIRE(parsed.ok());
  return parsed.value();
}

TEST_CASE("Json parses and dumps every value type") {
  const Json value = Parse(
      R"( {"a": [1, -2.5, true, false, null], "b": {"c": "x\ny"}, "e": 1e3} )");
  CHECK(value.is_object());
  const Json* a = value.GetArray("a");
  REQUIRE(a != nullptr);
  CHECK(a->size() == 5);
  CHECK(a->array_items()[0].is_integer());
  CHECK(a->array_items()[0].integer_value() == 1);
  CHECK(a->array_items()[1].number_value() == -2.5);
  CHECK(a->array_items()[2].bool_value());
  CHECK(a->array_items()[4].is_null());
  CHECK(*value.GetObject("b")->GetString("c") == "x\ny");
  CHECK_FALSE(value.Find("e")->is_integer());
  CHECK(value.GetInteger("e") == 1000);
  CHECK(value.Dump() ==
        R"({"a":[1,-2.5,true,false,null],"b":{"c":"x\ny"},"e":1000})");
}

TEST_CASE("Json keeps large integer ids exact") {
  const Json value = Parse("9007199254740993");
  CHECK(value.is_integer());
  CHECK(value.integer_value() == int64_t{9007199254740993});
  CHECK(value.Dump() == "9007199254740993");
}

TEST_CASE("Json decodes escapes and surrogate pairs") {
  const Json value = Parse(R"("\u00e9\ud83d\ude00\/\"\\\t")");
  CHECK(value.string_value() == "\xC3\xA9\xF0\x9F\x98\x80/\"\\\t");
  CHECK(Json::String("\x01").Dump() == "\"\\u0001\"");
}

TEST_CASE("Json rejects malformed input with an offset") {
  for (const char* bad : {"", "{", "[1,]", "{\"a\" 1}", "tru", "01", "1.",
                          "\"\\x\"", "\"\\ud800\"", "\"\xC0\xAF\"", "\"a\nb\"",
                          "1 2", "{\"a\":1,}", "-", "[1 2]"}) {
    CAPTURE(bad);
    CHECK_FALSE(ParseJson(bad).ok());
  }
  auto parsed = ParseJson("[1, x]");
  REQUIRE_FALSE(parsed.ok());
  CHECK(parsed.error().offset == 4);
}

TEST_CASE("Json limits nesting depth") {
  std::string deep(200, '[');
  deep += std::string(200, ']');
  CHECK_FALSE(ParseJson(deep).ok());
  std::string ok(100, '[');
  ok += std::string(100, ']');
  CHECK(ParseJson(ok).ok());
}

TEST_CASE("Json builder sets, replaces and erases members") {
  Json object = Json::Object();
  object.Set("name", "crate").Set("count", 3).Set("scale", 1.5);
  object.Set("count", int64_t{4});
  object.Set("flag", true);
  CHECK(object.size() == 4);
  CHECK(object.GetInteger("count") == 4);
  CHECK(object.GetBool("flag") == true);
  CHECK(object.GetString("missing") == nullptr);
  CHECK(object.GetNumber("name") == std::nullopt);
  CHECK(object.Erase("flag"));
  CHECK_FALSE(object.Erase("flag"));
  Json list;
  list.Push(Json::Integer(1)).Push(Json::Null());
  CHECK(list.Dump() == "[1,null]");
  CHECK(object.Dump() == R"({"name":"crate","count":4,"scale":1.5})");
  CHECK(Parse(object.DumpPretty()) == object);
}

TEST_CASE("Json equality treats integer and float forms alike") {
  CHECK(Json::Integer(2) == Json::Number(2.0));
  CHECK_FALSE(Json::Integer(2) == Json::String("2"));
  CHECK(Parse(R"({"a":[1,{"b":null}]})") == Parse(R"({"a":[1.0,{"b":null}]})"));
}

TEST_CASE("EncodeBase64 pads per RFC 4648") {
  CHECK(EncodeBase64("", 0).empty());
  CHECK(EncodeBase64("f", 1) == "Zg==");
  CHECK(EncodeBase64("fo", 2) == "Zm8=");
  CHECK(EncodeBase64("foo", 3) == "Zm9v");
  CHECK(EncodeBase64("foobar", 6) == "Zm9vYmFy");
}

TEST_CASE("Schema validation checks required members and basic types") {
  const Json schema = Parse(R"({
    "type": "object",
    "properties": {
      "path": {"type": "string"},
      "count": {"type": "integer"},
      "mode": {"type": "string", "enum": ["prefab", "scene"]},
      "position": {"type": "array", "items": {"type": "number"}},
      "light": {"type": "object", "required": ["color"],
                "properties": {"color": {"type": "array"}}}
    },
    "required": ["path"]
  })");
  CHECK_FALSE(ValidateAgainstSchema(schema, Parse(R"({"path":"a"})")));
  CHECK(ValidateAgainstSchema(schema, Parse(R"({})")).value().find("path") !=
        std::string::npos);
  CHECK(ValidateAgainstSchema(schema, Parse(R"({"path":1})")));
  CHECK(ValidateAgainstSchema(schema, Parse(R"({"path":"a","count":1.5})")));
  CHECK_FALSE(
      ValidateAgainstSchema(schema, Parse(R"({"path":"a","count":2.0})")));
  CHECK(ValidateAgainstSchema(schema, Parse(R"({"path":"a","mode":"x"})")));
  CHECK_FALSE(
      ValidateAgainstSchema(schema, Parse(R"({"path":"a","mode":"scene"})")));
  CHECK(ValidateAgainstSchema(schema,
                              Parse(R"({"path":"a","position":[1,"2"]})")));
  CHECK(ValidateAgainstSchema(schema, Parse(R"({"path":"a","light":{}})")));
  CHECK_FALSE(ValidateAgainstSchema(
      schema, Parse(R"({"path":"a","light":{"color":[1,1,1]}})")));
}

TEST_CASE("ToolResult helpers fill content") {
  const ToolResult error = ToolResult::Error("bad");
  CHECK(error.is_error);
  REQUIRE(error.content.size() == 1);
  CHECK(error.content[0].text == "bad");
  ToolResult structured = ToolResult::Structured(Json::Object().Set("a", 1));
  CHECK(structured.structured.has_value());
  CHECK(structured.content[0].text.find("\"a\": 1") != std::string::npos);
  structured.AddImage("AAAA", "image/png");
  CHECK(structured.content[1].kind == ContentItem::Kind::kImage);
}

}  // namespace
}  // namespace blue::mcp
