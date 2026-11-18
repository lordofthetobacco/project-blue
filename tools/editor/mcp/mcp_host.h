#ifndef BLUE_EDITOR_MCP_MCP_HOST_H_
#define BLUE_EDITOR_MCP_MCP_HOST_H_

#include <memory>
#include <string>

#include "core/result.h"
#include "editor/editor_settings.h"
#include "mcp/server.h"

namespace blue::editor {

class EditorContext;
class EditorTools;

// The editor's MCP server (Edit > Settings... > MCP server): an
// mcp::Server with the editor's tools (EditorTools), started and stopped
// to match McpSettings at runtime, polled once per frame on the main thread
// so tool handlers run there and never block on the network. Persisting the
// settings is the caller's business.
//
// Borrows `context`, which must outlive it. Neither copyable nor movable.
// Main thread only, except Status() (thread-safe like mcp::Server's).
class McpHost {
 public:
  explicit McpHost(EditorContext& context);
  // Stops the server.
  ~McpHost();
  McpHost(const McpHost&) = delete;
  McpHost& operator=(const McpHost&) = delete;

  // Starts, restarts (port changed or a previous start failed) or stops
  // the server so it matches `settings`. Returns why starting failed (also
  // shown by Status()); the settings count as applied either way.
  core::Result<void, std::string> Apply(const McpSettings& settings);
  [[nodiscard]] const McpSettings& applied() const { return applied_; }

  // Runs the queued tool calls and advances pending ones; call once per
  // frame between frames (not while ImGui builds the UI). Cheap while the
  // server is stopped or idle.
  void Update();

  [[nodiscard]] mcp::ServerStatus Status() const { return server_->Status(); }
  [[nodiscard]] bool listening() const { return server_->running(); }

 private:
  EditorContext& context_;
  McpSettings applied_;
  std::unique_ptr<mcp::Server> server_;
  std::unique_ptr<EditorTools> tools_;
};

}  // namespace blue::editor

#endif  // BLUE_EDITOR_MCP_MCP_HOST_H_
