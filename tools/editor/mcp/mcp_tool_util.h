#ifndef BLUE_EDITOR_MCP_MCP_TOOL_UTIL_H_
#define BLUE_EDITOR_MCP_MCP_TOOL_UTIL_H_

#include <chrono>
#include <functional>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"
#include "editor/document.h"
#include "mcp/json.h"
#include "mcp/server.h"
#include "mcp/tool.h"

// Helpers shared by the editor's MCP tool files (mcp_tools.cc,
// mcp_scene_tools.cc, mcp_prefab_tools.cc). Main thread only.

namespace blue::editor {

class EditorContext;

namespace mcp_tools {

using mcp::Json;
using mcp::ToolCall;
using mcp::ToolResult;
using ToolReturn = std::optional<ToolResult>;

// {"type": "object", "properties": ..., "required": [...],
//  "additionalProperties": false}.
[[nodiscard]] Json ObjectSchema(
    Json properties, std::initializer_list<std::string_view> required = {});
// {"type": `type`, "description": `description`}.
[[nodiscard]] Json Prop(std::string_view type, std::string_view description);
// A property without a type (e.g. "a name or an index").
[[nodiscard]] Json AnyProp(std::string_view description);
// An array of `count` numbers.
[[nodiscard]] Json VectorProp(int count, std::string_view description);
// A string property limited to `values`.
[[nodiscard]] Json EnumProp(std::initializer_list<std::string_view> values,
                            std::string_view description);

// What registering one tool takes besides its handler.
struct ToolSpec {
  std::string name;
  std::string title;
  std::string description;
  Json input_schema;
  bool read_only = false;
  bool destructive = false;
  std::chrono::milliseconds timeout = mcp::kDefaultToolTimeout;
};

// Registers a tool whose handler runs with error modals off (errors go to
// the result and the status bar, not a modal the user must close).
void AddTool(mcp::Server& server, EditorContext& context, ToolSpec spec,
             std::function<ToolReturn(const ToolCall&)> handler);

// Returns `path` made absolute: an absolute path stays; a relative one is
// taken relative to the asset root (also when the file does not exist yet).
[[nodiscard]] std::string ResolveAssetPath(const EditorContext& context,
                                           std::string_view path);

// The outcome of running editor command lines (CommandRegistry::Run) with
// the status and error messages they produced.
struct CommandOutcome {
  bool ok = true;
  std::string error;
  std::vector<std::string> messages;
};
[[nodiscard]] CommandOutcome RunCommandLine(EditorContext& context,
                                            std::string_view line);
// Turns `outcome` into a tool result: an error result with the error and
// messages, or `success` followed by the messages.
[[nodiscard]] ToolResult OutcomeResult(const CommandOutcome& outcome,
                                       std::string_view success);

// The open documents, or an error result naming what is missing.
[[nodiscard]] core::Result<SceneDocument*, ToolResult> RequireScene(
    EditorContext& context);
[[nodiscard]] core::Result<PrefabDocument*, ToolResult> RequirePrefab(
    EditorContext& context);

// Returns "Undo: <label>" for the document's newest undo step.
template <typename State>
[[nodiscard]] std::string UndoNote(const Document<State>& document) {
  return document.history().can_undo()
             ? "Undo step: " + std::string(document.history().undo_label())
             : std::string();
}

// The editor's state as editor_get_state returns it.
[[nodiscard]] Json EditorStateJson(EditorContext& context);

// Per-area registration (one file each).
void RegisterSceneTools(mcp::Server& server, EditorContext& context);
void RegisterPrefabTools(mcp::Server& server, EditorContext& context);

}  // namespace mcp_tools
}  // namespace blue::editor

#endif  // BLUE_EDITOR_MCP_MCP_TOOL_UTIL_H_
