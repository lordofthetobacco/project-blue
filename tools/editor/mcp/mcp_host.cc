#include "editor/mcp/mcp_host.h"

#include <memory>
#include <string>

#include "core/log.h"
#include "core/result.h"
#include "editor/editor_context.h"
#include "editor/editor_settings.h"
#include "editor/mcp/mcp_tools.h"
#include "mcp/server.h"

namespace blue::editor {
namespace {

// Tool calls run per frame at most; the rest wait for the next frame, so a
// burst of calls cannot stall the UI.
constexpr int kMaxCallsPerFrame = 8;

}  // namespace

McpHost::McpHost(EditorContext& context)
    : context_(context),
      server_(std::make_unique<mcp::Server>(
          mcp::ServerOptions{.instructions = EditorMcpInstructions()})),
      tools_(std::make_unique<EditorTools>(context_, *server_)) {}

McpHost::~McpHost() {
  // No handler may run once the tools are gone.
  server_->Stop();
}

core::Result<void, std::string> McpHost::Apply(const McpSettings& settings) {
  using VoidResult = core::Result<void, std::string>;
  const bool port_changed = settings.port != applied_.port;
  applied_ = settings;
  if (!settings.enabled) {
    if (server_->running()) {
      server_->Stop();
      context_.Status("MCP server stopped");
    }
    return VoidResult::Ok();
  }
  if (server_->running() && !port_changed) {
    return VoidResult::Ok();
  }
  VoidResult started = server_->Start(settings.port);
  if (!started.ok()) {
    core::log::Warn("MCP server: {}", started.error());
    context_.Status("MCP server: " + started.error());
    return started;
  }
  context_.Status("MCP server listening on " + McpUrl(server_->port()));
  return VoidResult::Ok();
}

void McpHost::Update() {
  if (server_->running()) {
    (void)server_->Poll(kMaxCallsPerFrame);
  }
  // Also while stopped: pending calls finish (CompleteCall ignores the ones
  // Stop() failed) so their readbacks are released.
  tools_->Update();
}

}  // namespace blue::editor
