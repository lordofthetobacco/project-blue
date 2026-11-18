#ifndef BLUE_MCP_TOOL_H_
#define BLUE_MCP_TOOL_H_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"
#include "mcp/json.h"

// The host-facing description of MCP tools and resources: what the host
// registers with mcp::Server and what its handlers return. No networking.
namespace blue::mcp {

// One item of a tool result's `content` array.
struct ContentItem {
  enum class Kind { kText, kImage };

  Kind kind = Kind::kText;
  // kText: the text.
  std::string text;
  // kImage: base64-encoded image bytes (see EncodeBase64) and their MIME
  // type, e.g. "image/png".
  std::string data;
  std::string mime_type;
};

// The result of one tool call (MCP CallToolResult).
struct ToolResult {
  // Returns a successful result with one text item.
  static ToolResult Text(std::string text);
  // Returns a failed result (isError = true) with one text item explaining
  // the failure. Use it for problems the model can react to: bad argument
  // values, missing documents, failed edits.
  static ToolResult Error(std::string message);
  // Returns a successful result whose structuredContent is `value` (an
  // object) and whose text item is the same JSON, pretty-printed, for clients
  // that read only text.
  static ToolResult Structured(Json value);

  // Appends an image item; `base64_data` as returned by EncodeBase64.
  ToolResult& AddImage(std::string base64_data, std::string mime_type);
  // Appends a text item.
  ToolResult& AddText(std::string text);

  std::vector<ContentItem> content;
  // Sent as structuredContent when the client's protocol revision supports
  // it (2025-06-18 and later); must be an object. Tools that declare an
  // output schema must fill it.
  std::optional<Json> structured;
  bool is_error = false;
};

// One tools/call request as seen by a handler. Valid only during the handler
// call; copy what a pending call needs later (keep `id`).
struct ToolCall {
  // Identifies the call for Server::CompleteCall. Unique per server.
  uint64_t id = 0;
  std::string_view name;
  // The arguments, already checked against the tool's input schema
  // (required members and basic types). Always an object.
  const Json& arguments;
};

// A tool handler. Runs on the host thread inside Server::Poll. Returns the
// result, or std::nullopt to mark the call pending: the host then finishes
// it later (on any later frame, from any thread) with
// Server::CompleteCall(call.id, result). A pending call that is not completed
// within the tool's timeout fails with a timeout error and a late
// CompleteCall is ignored.
using ToolHandler = std::function<std::optional<ToolResult>(const ToolCall&)>;

// The default time a client waits for a tool result before getting a
// timeout error.
inline constexpr std::chrono::milliseconds kDefaultToolTimeout{30000};

// A tool the server exposes (MCP Tool).
struct ToolDefinition {
  // Unique name, [A-Za-z0-9_.-], 1-128 characters, e.g. "scene_get".
  std::string name;
  // Short human-readable name, e.g. "Get scene".
  std::string title;
  // What the tool does, when to use it and what it returns: the model reads
  // this to decide how to call it.
  std::string description;
  // JSON Schema of the arguments; must be {"type":"object",...}. Calls are
  // validated against `required` and the `type` of each listed property
  // (string, number, integer, boolean, object, array, null; nested objects
  // and array items one level deep are checked the same way) before the
  // handler runs.
  Json input_schema;
  // Optional JSON Schema of structuredContent (2025-06-18 and later).
  std::optional<Json> output_schema;
  // MCP tool annotations (hints for clients): set read_only for tools that
  // do not modify anything, destructive for ones that delete or overwrite.
  bool read_only = false;
  bool destructive = false;
  ToolHandler handler;
  // How long the client waits for the result (including pending time).
  std::chrono::milliseconds timeout = kDefaultToolTimeout;
};

// The content of a resource (MCP TextResourceContents).
struct ResourceContents {
  std::string mime_type;
  std::string text;
};

// A resource handler. Runs on the host thread inside Server::Poll and
// returns the resource's current contents, or an error message (sent as a
// JSON-RPC internal error).
using ResourceHandler =
    std::function<core::Result<ResourceContents, std::string>(
        std::string_view uri)>;

// A fixed-URI resource the server exposes (MCP Resource).
struct ResourceDefinition {
  // e.g. "blue://scene".
  std::string uri;
  std::string name;
  std::string title;
  std::string description;
  std::string mime_type;
  ResourceHandler handler;
};

// Returns an error message when `arguments` does not satisfy `schema` (the
// subset described at ToolDefinition::input_schema), std::nullopt when it
// does. Exposed for tests and for hosts that validate nested data.
std::optional<std::string> ValidateAgainstSchema(const Json& schema,
                                                 const Json& arguments);

}  // namespace blue::mcp

#endif  // BLUE_MCP_TOOL_H_
