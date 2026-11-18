#include <doctest/doctest.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "core/result.h"
#include "mcp/http.h"
#include "mcp/json.h"
#include "mcp/protocol.h"
#include "mcp/server.h"
#include "mcp/tool.h"

namespace blue::mcp {
namespace {

// Runs handlers inline on the calling thread.
class InlineHost final : public HostBridge {
 public:
  ToolResult CallTool(std::shared_ptr<const ToolDefinition> tool,
                      Json arguments) override {
    ++calls;
    const ToolCall call{7, tool->name, arguments};
    std::optional<ToolResult> result = tool->handler(call);
    return result ? std::move(*result) : ToolResult::Error("pending");
  }
  core::Result<ResourceContents, std::string> ReadResource(
      std::shared_ptr<const ResourceDefinition> resource) override {
    return resource->handler(resource->uri);
  }

  int calls = 0;
};

Json Parse(const std::string& text) {
  auto parsed = ParseJson(text);
  REQUIRE(parsed.ok());
  return parsed.value();
}

// A protocol fixture with one "echo" tool and one resource.
class Fixture {
 public:
  Fixture() : endpoint_(&options_, &registry_, &activity_) {
    options_.instructions = "Use the tools.";
    ToolDefinition echo;
    echo.name = "echo";
    echo.title = "Echo";
    echo.description = "Echoes text.";
    echo.input_schema =
        Parse(R"({"type":"object","properties":{"text":{"type":"string"}},)"
              R"("required":["text"]})");
    echo.output_schema = Parse(R"({"type":"object"})");
    echo.read_only = true;
    echo.handler = [](const ToolCall& call) -> std::optional<ToolResult> {
      const std::string text = *call.arguments.GetString("text");
      if (text == "fail") {
        return ToolResult::Error("failed on purpose");
      }
      return ToolResult::Structured(Json::Object().Set("echo", text));
    };
    registry_.AddTool(std::move(echo));
    ResourceDefinition scene;
    scene.uri = "blue://scene";
    scene.name = "scene";
    scene.mime_type = "application/json";
    scene.handler =
        [](std::string_view) -> core::Result<ResourceContents, std::string> {
      return core::Result<ResourceContents, std::string>::Ok(
          ResourceContents{"application/json", "{\"instances\":[]}"});
    };
    registry_.AddResource(std::move(scene));
  }

  // POSTs `body` with the given extra headers.
  HttpResponse Post(const std::string& body,
                    std::vector<HttpHeader> headers = {}) {
    HttpRequest request;
    request.method = "POST";
    request.target = "/mcp";
    request.version = "HTTP/1.1";
    request.headers = {{"Host", "127.0.0.1:8765"},
                       {"Content-Type", "application/json"},
                       {"Accept", "application/json, text/event-stream"}};
    for (HttpHeader& header : headers) {
      request.headers.push_back(std::move(header));
    }
    request.body = body;
    return endpoint_.Handle(request, host_);
  }

  // Runs initialize at `version`; returns the session id.
  std::string Initialize(const std::string& version) {
    const HttpResponse response = Post(
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{)"
        R"("protocolVersion":")" +
        version +
        R"(","capabilities":{},"clientInfo":{"name":"test","version":"1"}}})");
    REQUIRE(response.status == 200);
    REQUIRE(response.FindHeader("Mcp-Session-Id") != nullptr);
    return *response.FindHeader("Mcp-Session-Id");
  }

  // A modern request body for `method` with `params_json` members.
  static std::string Modern(const std::string& method,
                            const std::string& extra_params = "") {
    return R"({"jsonrpc":"2.0","id":"r1","method":")" + method +
           R"(","params":{)" + extra_params +
           R"("_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28",)"
           R"("io.modelcontextprotocol/clientCapabilities":{}}}})";
  }

  static std::vector<HttpHeader> ModernHeaders(const std::string& method,
                                               const std::string& name = "") {
    std::vector<HttpHeader> headers = {{"MCP-Protocol-Version", "2026-07-28"},
                                       {"Mcp-Method", method}};
    if (!name.empty()) {
      headers.push_back({"Mcp-Name", name});
    }
    return headers;
  }

  Endpoint& endpoint() { return endpoint_; }
  InlineHost& host() { return host_; }

 private:
  ServerOptions options_;
  Registry registry_;
  ActivityLog activity_;
  Endpoint endpoint_;
  InlineHost host_;
};

Json Body(const HttpResponse& response) { return Parse(response.body); }

int ErrorCode(const HttpResponse& response) {
  const Json body = Body(response);
  const Json* error = body.GetObject("error");
  REQUIRE(error != nullptr);
  return static_cast<int>(error->GetInteger("code").value_or(0));
}

TEST_CASE("Legacy lifecycle: initialize, initialized, list and call") {
  Fixture fixture;
  const HttpResponse init =
      fixture.Post(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{)"
                   R"("protocolVersion":"2025-11-25","capabilities":{},)"
                   R"("clientInfo":{"name":"claude-code","version":"2.0"}}})");
  REQUIRE(init.status == 200);
  const Json result = *Body(init).GetObject("result");
  CHECK(*result.GetString("protocolVersion") == "2025-11-25");
  CHECK(*result.GetObject("serverInfo")->GetString("name") == "blue-editor");
  CHECK(*result.GetString("instructions") == "Use the tools.");
  CHECK(result.GetObject("capabilities")->GetObject("tools") != nullptr);
  const std::string session = *init.FindHeader("Mcp-Session-Id");
  CHECK(session.size() == 32);
  CHECK(fixture.endpoint().session_count() == 1);

  const std::vector<HttpHeader> headers = {
      {"Mcp-Session-Id", session}, {"MCP-Protocol-Version", "2025-11-25"}};
  const HttpResponse initialized = fixture.Post(
      R"({"jsonrpc":"2.0","method":"notifications/initialized"})", headers);
  CHECK(initialized.status == 202);
  CHECK(initialized.body.empty());

  const HttpResponse list = fixture.Post(
      R"({"jsonrpc":"2.0","id":"2","method":"tools/list"})", headers);
  REQUIRE(list.status == 200);
  const Json tools = Body(list);
  CHECK(*tools.GetString("id") == "2");
  const Json& tool =
      tools.GetObject("result")->GetArray("tools")->array_items()[0];
  CHECK(*tool.GetString("name") == "echo");
  CHECK(*tool.GetString("title") == "Echo");
  CHECK(tool.GetObject("outputSchema") != nullptr);
  CHECK(tool.GetObject("annotations")->GetBool("readOnlyHint") == true);

  const HttpResponse call =
      fixture.Post(R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{)"
                   R"("name":"echo","arguments":{"text":"hi"}}})",
                   headers);
  REQUIRE(call.status == 200);
  const Json call_result = *Body(call).GetObject("result");
  CHECK(call_result.GetBool("isError") == false);
  CHECK(*call_result.GetObject("structuredContent")->GetString("echo") == "hi");
  CHECK(call_result.GetArray("content")->size() == 1);
  CHECK(fixture.endpoint().tool_call_count() == 1);

  const HttpResponse ping =
      fixture.Post(R"({"jsonrpc":"2.0","id":4,"method":"ping"})", headers);
  CHECK(Body(ping).GetObject("result") != nullptr);
}

TEST_CASE("Legacy version negotiation and per-revision fields") {
  Fixture fixture;
  const HttpResponse unknown =
      fixture.Post(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{)"
                   R"("protocolVersion":"1999-01-01","capabilities":{}}})");
  CHECK(*Body(unknown).GetObject("result")->GetString("protocolVersion") ==
        "2025-11-25");

  const std::string old_session = fixture.Initialize("2025-03-26");
  const HttpResponse list =
      fixture.Post(R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})",
                   {{"Mcp-Session-Id", old_session}});
  const Json tool =
      Body(list).GetObject("result")->GetArray("tools")->array_items()[0];
  CHECK(tool.Find("title") == nullptr);
  CHECK(tool.Find("outputSchema") == nullptr);
  CHECK(tool.Find("annotations") != nullptr);
  const HttpResponse call =
      fixture.Post(R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{)"
                   R"("name":"echo","arguments":{"text":"x"}}})",
                   {{"Mcp-Session-Id", old_session}});
  CHECK(Body(call).GetObject("result")->Find("structuredContent") == nullptr);
}

TEST_CASE("Legacy sessions are required, checked and deletable") {
  Fixture fixture;
  const std::string body = R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})";
  CHECK(fixture.Post(body).status == 400);
  CHECK(fixture.Post(body, {{"Mcp-Session-Id", "nope"}}).status == 404);
  const std::string session = fixture.Initialize("2025-06-18");
  CHECK(fixture
            .Post(body, {{"Mcp-Session-Id", session},
                         {"MCP-Protocol-Version", "2030-01-01"}})
            .status == 400);
  CHECK(fixture.Post(body, {{"Mcp-Session-Id", session}}).status == 200);

  HttpRequest del;
  del.method = "DELETE";
  del.target = "/mcp";
  del.headers = {{"Host", "localhost:8765"}, {"Mcp-Session-Id", session}};
  CHECK(fixture.endpoint().Handle(del, fixture.host()).status == 204);
  CHECK(fixture.endpoint().Handle(del, fixture.host()).status == 404);
  CHECK(fixture.Post(body, {{"Mcp-Session-Id", session}}).status == 404);
}

TEST_CASE("Legacy batches only in 2025-03-26 sessions") {
  Fixture fixture;
  const std::string batch =
      R"([{"jsonrpc":"2.0","id":1,"method":"ping"},)"
      R"({"jsonrpc":"2.0","method":"notifications/initialized"},)"
      R"({"jsonrpc":"2.0","id":2,"method":"nope"}])";
  const std::string new_session = fixture.Initialize("2025-11-25");
  CHECK(fixture.Post(batch, {{"Mcp-Session-Id", new_session}}).status == 400);
  const std::string old_session = fixture.Initialize("2025-03-26");
  const HttpResponse response =
      fixture.Post(batch, {{"Mcp-Session-Id", old_session}});
  REQUIRE(response.status == 200);
  const Json body = Body(response);
  REQUIRE(body.size() == 2);
  CHECK(body.array_items()[0].GetObject("result") != nullptr);
  CHECK(body.array_items()[1].GetObject("error")->GetInteger("code") ==
        kMethodNotFound);
}

TEST_CASE("JSON-RPC errors: parse, invalid request, method, params") {
  Fixture fixture;
  const HttpResponse parse = fixture.Post("{not json");
  CHECK(parse.status == 400);
  CHECK(ErrorCode(parse) == kParseError);
  CHECK(Body(parse).Find("id") == nullptr);

  const std::string session = fixture.Initialize("2025-11-25");
  const std::vector<HttpHeader> headers = {{"Mcp-Session-Id", session}};
  const HttpResponse invalid =
      fixture.Post(R"({"jsonrpc":"1.0","id":1,"method":"ping"})", headers);
  CHECK(invalid.status == 400);
  CHECK(ErrorCode(invalid) == kInvalidRequest);
  CHECK(ErrorCode(fixture.Post(R"({"jsonrpc":"2.0","id":null,"method":"x"})",
                               headers)) == kInvalidRequest);

  const HttpResponse missing =
      fixture.Post(R"({"jsonrpc":"2.0","id":5,"method":"nope/nope"})", headers);
  CHECK(missing.status == 200);
  CHECK(ErrorCode(missing) == kMethodNotFound);
  CHECK(Body(missing).GetInteger("id") == 5);

  const HttpResponse unknown_tool = fixture.Post(
      R"({"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"zz"}})",
      headers);
  CHECK(ErrorCode(unknown_tool) == kInvalidParams);

  const HttpResponse bad_args =
      fixture.Post(R"({"jsonrpc":"2.0","id":7,"method":"tools/call","params":{)"
                   R"("name":"echo","arguments":{"text":5}}})",
                   headers);
  const Json result = *Body(bad_args).GetObject("result");
  CHECK(result.GetBool("isError") == true);
  CHECK(result.GetArray("content")->array_items()[0].GetString("text")->find(
            "arguments.text must be string") != std::string::npos);
  CHECK(fixture.host().calls == 0);

  const HttpResponse tool_error =
      fixture.Post(R"({"jsonrpc":"2.0","id":8,"method":"tools/call","params":{)"
                   R"("name":"echo","arguments":{"text":"fail"}}})",
                   headers);
  CHECK(Body(tool_error).GetObject("result")->GetBool("isError") == true);

  const HttpResponse no_resource = fixture.Post(
      R"({"jsonrpc":"2.0","id":9,"method":"resources/read","params":{)"
      R"("uri":"blue://none"}})",
      headers);
  CHECK(ErrorCode(no_resource) == kLegacyResourceNotFound);
}

TEST_CASE("Legacy resources list and read") {
  Fixture fixture;
  const std::vector<HttpHeader> headers = {
      {"Mcp-Session-Id", fixture.Initialize("2025-11-25")}};
  const HttpResponse list = fixture.Post(
      R"({"jsonrpc":"2.0","id":1,"method":"resources/list"})", headers);
  const Json resource =
      Body(list).GetObject("result")->GetArray("resources")->array_items()[0];
  CHECK(*resource.GetString("uri") == "blue://scene");
  const HttpResponse read = fixture.Post(
      R"({"jsonrpc":"2.0","id":2,"method":"resources/read","params":{)"
      R"("uri":"blue://scene"}})",
      headers);
  const Json contents =
      Body(read).GetObject("result")->GetArray("contents")->array_items()[0];
  CHECK(*contents.GetString("text") == "{\"instances\":[]}");
  CHECK(*contents.GetString("mimeType") == "application/json");
  const HttpResponse templates = fixture.Post(
      R"({"jsonrpc":"2.0","id":3,"method":"resources/templates/list"})",
      headers);
  CHECK(Body(templates)
            .GetObject("result")
            ->GetArray("resourceTemplates")
            ->size() == 0);
}

TEST_CASE("Modern requests are stateless and carry resultType") {
  Fixture fixture;
  const HttpResponse discover =
      fixture.Post(Fixture::Modern("server/discover"),
                   Fixture::ModernHeaders("server/discover"));
  REQUIRE(discover.status == 200);
  const Json result = *Body(discover).GetObject("result");
  CHECK(*result.GetString("resultType") == "complete");
  CHECK(result.GetArray("supportedVersions")->array_items()[0].string_value() ==
        "2026-07-28");
  CHECK(*result.GetObject("_meta")
             ->GetObject("io.modelcontextprotocol/serverInfo")
             ->GetString("name") == "blue-editor");
  CHECK(result.Find("ttlMs") != nullptr);
  CHECK(discover.FindHeader("Mcp-Session-Id") == nullptr);

  const HttpResponse call = fixture.Post(
      Fixture::Modern("tools/call",
                      R"("name":"echo","arguments":{"text":"m"},)"),
      Fixture::ModernHeaders("tools/call", "echo"));
  REQUIRE(call.status == 200);
  const Json call_result = *Body(call).GetObject("result");
  CHECK(*call_result.GetString("resultType") == "complete");
  CHECK(*call_result.GetObject("structuredContent")->GetString("echo") == "m");

  // Base64-encoded Mcp-Name is decoded before comparing.
  const HttpResponse encoded = fixture.Post(
      Fixture::Modern("tools/call",
                      R"("name":"echo","arguments":{"text":"m"},)"),
      Fixture::ModernHeaders("tools/call", "=?base64?ZWNobw==?="));
  CHECK(encoded.status == 200);

  const HttpResponse read = fixture.Post(
      Fixture::Modern("resources/read", R"("uri":"blue://scene",)"),
      Fixture::ModernHeaders("resources/read", "blue://scene"));
  CHECK(Body(read).GetObject("result")->GetArray("contents")->size() == 1);
  const HttpResponse missing =
      fixture.Post(Fixture::Modern("resources/read", R"("uri":"blue://x",)"),
                   Fixture::ModernHeaders("resources/read", "blue://x"));
  CHECK(ErrorCode(missing) == kInvalidParams);

  const HttpResponse notification = fixture.Post(
      R"({"jsonrpc":"2.0","method":"notifications/whatever","params":{)"
      R"("_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}})");
  CHECK(notification.status == 202);
}

TEST_CASE("Modern request validation") {
  Fixture fixture;
  const HttpResponse unsupported = fixture.Post(
      R"({"jsonrpc":"2.0","id":1,"method":"tools/list","params":{"_meta":{)"
      R"("io.modelcontextprotocol/protocolVersion":"1900-01-01",)"
      R"("io.modelcontextprotocol/clientCapabilities":{}}}})",
      {{"MCP-Protocol-Version", "1900-01-01"}, {"Mcp-Method", "tools/list"}});
  CHECK(unsupported.status == 400);
  CHECK(ErrorCode(unsupported) == kUnsupportedProtocolVersion);
  const Json data = *Body(unsupported).GetObject("error")->GetObject("data");
  CHECK(*data.GetString("requested") == "1900-01-01");
  CHECK(data.GetArray("supported")->size() == 5);

  // Missing clientCapabilities.
  const HttpResponse no_caps = fixture.Post(
      R"({"jsonrpc":"2.0","id":1,"method":"tools/list","params":{"_meta":{)"
      R"("io.modelcontextprotocol/protocolVersion":"2026-07-28"}}})",
      Fixture::ModernHeaders("tools/list"));
  CHECK(no_caps.status == 400);
  CHECK(ErrorCode(no_caps) == kInvalidParams);

  // Modern version header without _meta.
  const HttpResponse no_meta =
      fixture.Post(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})",
                   Fixture::ModernHeaders("tools/list"));
  CHECK(no_meta.status == 400);
  CHECK(ErrorCode(no_meta) == kInvalidParams);

  // Header mismatches.
  CHECK(ErrorCode(fixture.Post(Fixture::Modern("tools/list"),
                               {{"Mcp-Method", "tools/list"}})) ==
        kHeaderMismatch);
  CHECK(ErrorCode(fixture.Post(Fixture::Modern("tools/list"),
                               Fixture::ModernHeaders("tools/call"))) ==
        kHeaderMismatch);
  CHECK(ErrorCode(fixture.Post(
            Fixture::Modern("tools/call",
                            R"("name":"echo","arguments":{"text":"m"},)"),
            Fixture::ModernHeaders("tools/call", "other"))) == kHeaderMismatch);
  CHECK(ErrorCode(fixture.Post(
            Fixture::Modern("tools/call",
                            R"("name":"echo","arguments":{"text":"m"},)"),
            Fixture::ModernHeaders("tools/call"))) == kHeaderMismatch);

  // Unknown methods (ping is legacy only) are 404 with -32601.
  const HttpResponse ping =
      fixture.Post(Fixture::Modern("ping"), Fixture::ModernHeaders("ping"));
  CHECK(ping.status == 404);
  CHECK(ErrorCode(ping) == kMethodNotFound);
}

TEST_CASE("Origin, Host, path and method checks") {
  Fixture fixture;
  const std::string body = Fixture::Modern("server/discover");
  const auto headers = Fixture::ModernHeaders("server/discover");
  std::vector<HttpHeader> evil = headers;
  evil.push_back({"Origin", "http://evil.example"});
  CHECK(fixture.Post(body, evil).status == 403);
  std::vector<HttpHeader> rebinding = headers;
  rebinding.push_back({"Origin", "http://localhost.evil.example:8765"});
  CHECK(fixture.Post(body, rebinding).status == 403);
  std::vector<HttpHeader> local = headers;
  local.push_back({"Origin", "http://localhost:3000"});
  CHECK(fixture.Post(body, local).status == 200);

  CHECK(IsLocalOrigin("http://127.0.0.1:6274"));
  CHECK(IsLocalOrigin("https://[::1]"));
  CHECK_FALSE(IsLocalOrigin("null"));
  CHECK_FALSE(IsLocalOrigin("http://127.0.0.1.nip.io"));
  CHECK(IsLocalHost("localhost"));
  CHECK(IsLocalHost("127.0.0.1:8765"));
  CHECK_FALSE(IsLocalHost("attacker.com:8765"));

  HttpRequest request;
  request.method = "POST";
  request.target = "/mcp";
  request.headers = {{"Host", "evil.example:8765"},
                     {"Content-Type", "application/json"}};
  request.body = body;
  CHECK(fixture.endpoint().Handle(request, fixture.host()).status == 403);

  request.headers[0].value = "127.0.0.1:8765";
  request.target = "/other";
  CHECK(fixture.endpoint().Handle(request, fixture.host()).status == 404);
  request.target = "/mcp";
  request.headers[1].value = "text/plain";
  CHECK(fixture.endpoint().Handle(request, fixture.host()).status == 415);
  request.method = "GET";
  CHECK(fixture.endpoint().Handle(request, fixture.host()).status == 405);
  request.method = "OPTIONS";
  const HttpResponse options =
      fixture.endpoint().Handle(request, fixture.host());
  CHECK(options.status == 204);
  CHECK(options.FindHeader("Allow") != nullptr);
}

TEST_CASE("Session count is capped") {
  Fixture fixture;
  for (size_t i = 0; i < Endpoint::kMaxSessions + 5; ++i) {
    fixture.Initialize("2025-11-25");
  }
  CHECK(fixture.endpoint().session_count() ==
        static_cast<int>(Endpoint::kMaxSessions));
}

}  // namespace
}  // namespace blue::mcp
