#ifndef BLUE_MCP_SERVER_H_
#define BLUE_MCP_SERVER_H_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/result.h"
#include "mcp/tool.h"

namespace blue::mcp {

// Static configuration of a server.
struct ServerOptions {
  // serverInfo of the initialize / server/discover result.
  std::string name = "blue-editor";
  std::string title = "Blue Editor";
  std::string version = "0.1.0";
  // Usage hints for the model (the result's `instructions`).
  std::string instructions;
  // Requests with a larger body get 413 Payload Too Large.
  size_t max_body_bytes = 8 * 1024 * 1024;
  // Further connections get 503 and are closed.
  int max_connections = 8;
  // An idle keep-alive connection is closed after this time.
  std::chrono::milliseconds idle_timeout{60000};
  // Timeout for resources/read requests waiting for Poll.
  std::chrono::milliseconds resource_timeout = kDefaultToolTimeout;
};

// One line of the recent-activity log.
struct ActivityEntry {
  std::chrono::system_clock::time_point time;
  // e.g. "tools/call scene_get (12 ms)", "rejected origin http://evil.com".
  std::string text;
};

// A snapshot of the server's state for the settings UI.
struct ServerStatus {
  enum class State { kStopped, kListening, kError };

  State state = State::kStopped;
  // The bound port while listening (the requested one, or the ephemeral
  // port chosen for 0); the last requested port otherwise.
  int port = 0;
  // Why Start failed or the listener died (State::kError).
  std::string last_error;
  // Currently open TCP connections, and connections accepted since the
  // Server was created (like the counters below, not reset by restarts).
  int open_connections = 0;
  int64_t total_connections = 0;
  // Live legacy (initialize-based) sessions.
  int sessions = 0;
  // JSON-RPC requests and notifications handled.
  int64_t requests = 0;
  int64_t tool_calls = 0;
  // Calls queued for Poll or pending on the host.
  int waiting_calls = 0;
  // The most recent activity, oldest first (at most 64 entries).
  std::vector<ActivityEntry> recent_activity;
};

// An MCP server (Streamable HTTP transport) on
// http://127.0.0.1:<port>/mcp, loopback only. It serves both the stateless
// modern protocol (2026-07-28: per-request _meta, server/discover) and the
// legacy initialize-based revisions (2025-11-25, 2025-06-18, 2025-03-26,
// 2024-11-05) with Mcp-Session-Id sessions.
//
// Threading contract:
//   - The host thread owns the Server: it constructs, registers tools and
//     resources, calls Start, Stop, Poll and destroys it. These methods must
//     not be called concurrently with each other.
//   - Start creates one listener thread plus one thread per open connection
//     (at most ServerOptions::max_connections). Network threads parse HTTP
//     and JSON-RPC and answer protocol requests (initialize, tools/list,
//     ping, ...) on their own; they never run host code.
//   - tools/call and resources/read are queued. The connection's thread
//     waits (up to the tool's timeout, interruptible by Stop) while the host
//     runs the handler inside Poll on its own thread. A handler may return
//     "pending" and finish later through CompleteCall.
//   - CompleteCall, Status, running and port are thread-safe.
//   - Stop (and the destructor) close every socket, fail all waiting calls
//     and join all threads; it returns within about 100 ms even while
//     requests are waiting for the host.
//
// Registration may happen before or after Start; tools/list reflects the
// registry at the time of the request. Handlers must stay valid until they
// are replaced or the server is destroyed.
//
// Neither copyable nor movable (network threads refer to it).
class Server {
 public:
  explicit Server(ServerOptions options);
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;
  // Stops the server (see Stop).
  ~Server();

  // Adds a tool, replacing one with the same name. Host thread.
  void RegisterTool(ToolDefinition tool);
  // Adds a resource, replacing one with the same URI. Host thread.
  void RegisterResource(ResourceDefinition resource);

  // Starts listening on 127.0.0.1:`port` (0 = ephemeral, see port()).
  // Restarts when already running. Fails with a readable message (and
  // State::kError in Status) when the port is in use or sockets fail.
  core::Result<void, std::string> Start(int port);
  // Stops listening, closes all connections and fails waiting calls. No-op
  // when stopped.
  void Stop();

  // True while listening.
  bool running() const;
  // The bound port while running, else the last requested port.
  int port() const;
  // The last error message, empty when none.
  std::string last_error() const;

  // Runs up to `max_calls` queued tool calls and resource reads on the
  // calling (host) thread; returns how many ran. Cheap when nothing is
  // queued: no allocation, one mutex lock.
  int Poll(int max_calls);

  // Finishes a call whose handler returned std::nullopt. Returns false when
  // the call is unknown, already finished, or timed out / was cancelled.
  // Thread-safe.
  bool CompleteCall(uint64_t call_id, ToolResult result);

  // Returns a copy of the current state. Thread-safe; allocates (UI use).
  ServerStatus Status() const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace blue::mcp

#endif  // BLUE_MCP_SERVER_H_
