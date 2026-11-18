#ifndef BLUE_MCP_PROTOCOL_H_
#define BLUE_MCP_PROTOCOL_H_

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"
#include "mcp/http.h"
#include "mcp/json.h"
#include "mcp/server.h"
#include "mcp/tool.h"

// The MCP layer of the server: JSON-RPC 2.0 over the Streamable HTTP
// transport, from a parsed HTTP request to the HTTP response. No sockets and
// no threads of its own; host work (tool handlers, resource reads) goes
// through a HostBridge, so tests drive it directly.
namespace blue::mcp {

// The stateless revision served through per-request _meta.
inline constexpr std::string_view kModernProtocolVersion = "2026-07-28";
// The initialize-based revisions, newest first. The first one is offered to
// clients that ask for a revision we do not know.
inline constexpr std::string_view kLegacyProtocolVersions[] = {
    "2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"};

// The MCP and JSON-RPC error codes the endpoint emits.
inline constexpr int kParseError = -32700;
inline constexpr int kInvalidRequest = -32600;
inline constexpr int kMethodNotFound = -32601;
inline constexpr int kInvalidParams = -32602;
inline constexpr int kInternalError = -32603;
inline constexpr int kLegacyResourceNotFound = -32002;
inline constexpr int kHeaderMismatch = -32020;
inline constexpr int kUnsupportedProtocolVersion = -32022;

// Registered tools and resources. Thread-safe: the host adds entries while
// network threads list and look them up. Definitions are shared immutable
// snapshots, so a call keeps its definition alive even if it is replaced
// meanwhile.
class Registry {
 public:
  Registry() = default;
  Registry(const Registry&) = delete;
  Registry& operator=(const Registry&) = delete;

  // Adds or replaces (same name / URI).
  void AddTool(ToolDefinition tool);
  void AddResource(ResourceDefinition resource);

  // Returns all tools sorted by name (deterministic tools/list order).
  std::vector<std::shared_ptr<const ToolDefinition>> Tools() const;
  // Returns all resources sorted by URI.
  std::vector<std::shared_ptr<const ResourceDefinition>> Resources() const;
  // Returns the tool / resource, or nullptr.
  std::shared_ptr<const ToolDefinition> FindTool(std::string_view name) const;
  std::shared_ptr<const ResourceDefinition> FindResource(
      std::string_view uri) const;

 private:
  mutable std::mutex mutex_;
  std::map<std::string, std::shared_ptr<const ToolDefinition>, std::less<>>
      tools_;
  std::map<std::string, std::shared_ptr<const ResourceDefinition>, std::less<>>
      resources_;
};

// Runs host work for the endpoint; implemented by the server's call queue
// (and by tests). Called on network threads; may block.
class HostBridge {
 public:
  virtual ~HostBridge() = default;

  // Returns the result of calling `tool` with schema-checked `arguments`.
  // Timeouts and shutdown come back as error results.
  virtual ToolResult CallTool(std::shared_ptr<const ToolDefinition> tool,
                              Json arguments) = 0;
  // Returns the contents of `resource`, or an error message.
  virtual core::Result<ResourceContents, std::string> ReadResource(
      std::shared_ptr<const ResourceDefinition> resource) = 0;
};

// A bounded, thread-safe log of recent activity for the settings UI.
class ActivityLog {
 public:
  static constexpr size_t kCapacity = 64;

  ActivityLog() = default;
  ActivityLog(const ActivityLog&) = delete;
  ActivityLog& operator=(const ActivityLog&) = delete;

  // Appends `text` (timestamped now), dropping the oldest entry when full.
  void Add(std::string text);
  // Returns the entries, oldest first.
  std::vector<ActivityEntry> Snapshot() const;
  void Clear();

 private:
  mutable std::mutex mutex_;
  std::deque<ActivityEntry> entries_;
};

// The MCP endpoint (path /mcp). Thread-safe: connection threads call Handle
// concurrently. Serves
//   - modern requests (2026-07-28): any request whose params._meta carries
//     io.modelcontextprotocol/protocolVersion. Stateless; the
//     MCP-Protocol-Version, Mcp-Method and Mcp-Name headers must match the
//     body (400 / -32020 otherwise). Methods: server/discover, tools/list,
//     tools/call, resources/list, resources/read, resources/templates/list.
//   - legacy requests: initialize creates a session (Mcp-Session-Id);
//     later requests must carry it (400 without, 404 when unknown); DELETE
//     ends it. Methods: the modern ones minus server/discover, plus ping.
//     JSON-RPC batches are accepted only in 2025-03-26 sessions.
// Requests with a non-local Origin or Host get 403 before anything else.
class Endpoint {
 public:
  // At most this many legacy sessions are kept; the least recently used one
  // is dropped when a new one starts.
  static constexpr size_t kMaxSessions = 32;

  // `options` and `registry` are borrowed and must outlive the endpoint;
  // `activity` too.
  Endpoint(const ServerOptions* options, const Registry* registry,
           ActivityLog* activity);
  Endpoint(const Endpoint&) = delete;
  Endpoint& operator=(const Endpoint&) = delete;

  // Returns the response to `request`. Runs tool calls and resource reads
  // through `host` (blocking this thread until they finish).
  HttpResponse Handle(const HttpRequest& request, HostBridge& host);

  // Forgets every legacy session (server restart).
  void ClearSessions();

  int session_count() const;
  // JSON-RPC messages handled (requests, notifications, batch elements).
  int64_t request_count() const { return requests_.load(); }
  int64_t tool_call_count() const { return tool_calls_.load(); }

 private:
  struct Session {
    std::string protocol_version;
    std::string client_name;
    std::chrono::steady_clock::time_point last_used;
  };
  struct CallContext;
  struct Outcome;

  HttpResponse HandlePost(const HttpRequest& request, HostBridge& host);
  HttpResponse HandleDelete(const HttpRequest& request);
  HttpResponse HandleModern(const HttpRequest& request, const Json& message,
                            HostBridge& host);
  HttpResponse HandleInitialize(const Json& message);
  HttpResponse HandleLegacy(const HttpRequest& request, const Json& message,
                            HostBridge& host);
  // Runs one legacy message; returns the response object or null for
  // notifications and client responses.
  Json HandleLegacyMessage(const Json& message, const CallContext& context,
                           HostBridge& host);
  Outcome Dispatch(std::string_view method, const Json& params,
                   const CallContext& context, HostBridge& host);
  Outcome CallTool(const Json& params, const CallContext& context,
                   HostBridge& host);
  Outcome ReadResource(const Json& params, const CallContext& context,
                       HostBridge& host);
  Json ListTools(const CallContext& context) const;
  Json ListResources(const CallContext& context) const;
  Json ServerInfo() const;
  Json Capabilities(bool legacy) const;

  // Returns the session's protocol version and marks it used; empty when
  // the session does not exist.
  std::string TouchSession(std::string_view id);
  std::string CreateSession(std::string protocol_version,
                            std::string client_name);

  const ServerOptions* options_;
  const Registry* registry_;
  ActivityLog* activity_;
  std::atomic<int64_t> requests_{0};
  std::atomic<int64_t> tool_calls_{0};
  mutable std::mutex sessions_mutex_;
  std::map<std::string, Session, std::less<>> sessions_;
};

// Returns whether `origin` (an Origin header value) names this machine:
// http(s)://localhost, 127.0.0.1 or [::1], any port.
bool IsLocalOrigin(std::string_view origin);
// Returns whether `host` (a Host header value) names this machine.
bool IsLocalHost(std::string_view host);
// Decodes a "=?base64?...?=" header value; other values are returned as
// they are. Returns false when the base64 part is malformed.
bool DecodeHeaderValue(std::string_view value, std::string& decoded);

}  // namespace blue::mcp

#endif  // BLUE_MCP_PROTOCOL_H_
