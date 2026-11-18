#include "editor/mcp/mcp_tool_util.h"

#include <deque>
#include <format>
#include <functional>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

#include "core/path.h"
#include "core/result.h"
#include "editor/document.h"
#include "editor/editor_context.h"
#include "editor/panel.h"
#include "editor/selection.h"
#include "mcp/json.h"
#include "mcp/server.h"
#include "mcp/tool.h"
#include "platform/file_system.h"

namespace blue::editor::mcp_tools {
namespace {

// Returns true for "C:\...", "C:/...", "\\server\..." and "/...".
bool IsAbsolutePath(std::string_view path) {
  if (path.size() >= 2 && path[1] == ':') {
    return true;
  }
  return !path.empty() && (path.front() == '/' || path.front() == '\\');
}

template <typename State>
Json DocumentJson(const Document<State>* document,
                  const std::string& asset_root) {
  if (document == nullptr) {
    return Json::Null();
  }
  const SnapshotHistory<State>& history = document->history();
  return Json::Object()
      .Set("path",
           document->path().empty()
               ? Json::Null()
               : Json::String(DisplayPath(document->path(), asset_root)))
      .Set("title", DocumentTitle(document->path()))
      .Set("dirty", document->dirty())
      .Set("undo", history.can_undo()
                       ? Json::String(std::string(history.undo_label()))
                       : Json::Null())
      .Set("redo", history.can_redo()
                       ? Json::String(std::string(history.redo_label()))
                       : Json::Null());
}

}  // namespace

Json ObjectSchema(Json properties,
                  std::initializer_list<std::string_view> required) {
  Json schema = Json::Object()
                    .Set("type", "object")
                    .Set("properties", std::move(properties));
  if (required.size() > 0) {
    Json list = Json::Array();
    for (const std::string_view name : required) {
      list.Push(Json::String(std::string(name)));
    }
    schema.Set("required", std::move(list));
  }
  schema.Set("additionalProperties", false);
  return schema;
}

Json Prop(std::string_view type, std::string_view description) {
  return Json::Object().Set("type", type).Set("description", description);
}

Json AnyProp(std::string_view description) {
  return Json::Object().Set("description", description);
}

Json VectorProp(int count, std::string_view description) {
  return Json::Object()
      .Set("type", "array")
      .Set("items", Json::Object().Set("type", "number"))
      .Set("minItems", count)
      .Set("maxItems", count)
      .Set("description", description);
}

Json EnumProp(std::initializer_list<std::string_view> values,
              std::string_view description) {
  Json list = Json::Array();
  for (const std::string_view value : values) {
    list.Push(Json::String(std::string(value)));
  }
  return Json::Object()
      .Set("type", "string")
      .Set("enum", std::move(list))
      .Set("description", description);
}

void AddTool(mcp::Server& server, EditorContext& context, ToolSpec spec,
             std::function<ToolReturn(const ToolCall&)> handler) {
  EditorContext* ctx = &context;
  server.RegisterTool(
      {.name = std::move(spec.name),
       .title = std::move(spec.title),
       .description = std::move(spec.description),
       .input_schema = std::move(spec.input_schema),
       .read_only = spec.read_only,
       .destructive = spec.destructive,
       .handler =
           [ctx, handler = std::move(handler)](const ToolCall& call) {
             ctx->set_error_modals(false);
             ToolReturn result = handler(call);
             ctx->set_error_modals(true);
             return result;
           },
       .timeout = spec.timeout});
}

std::string ResolveAssetPath(const EditorContext& context,
                             std::string_view path) {
  if (IsAbsolutePath(path)) {
    return std::string(path);
  }
  return core::JoinPath(context.asset_root(), path);
}

CommandOutcome RunCommandLine(EditorContext& context, std::string_view line) {
  const uint64_t first = context.next_message_serial();
  core::Result<void, std::string> result = context.commands().Run(line);
  CommandOutcome outcome;
  outcome.ok = result.ok();
  if (!result.ok()) {
    outcome.error = result.error();
  }
  for (const EditorContext::Message& message : context.messages()) {
    if (message.serial >= first) {
      outcome.messages.push_back(message.error ? "error: " + message.text
                                               : message.text);
      // An error dialog counts as a failure even if the command returned
      // success (e.g. SaveActive() reports through a modal).
      if (message.error && outcome.ok) {
        outcome.ok = false;
        outcome.error = message.text;
      }
    }
  }
  return outcome;
}

ToolResult OutcomeResult(const CommandOutcome& outcome,
                         std::string_view success) {
  std::string text = outcome.ok ? std::string(success) : outcome.error;
  for (const std::string& message : outcome.messages) {
    text += "\n" + message;
  }
  return outcome.ok ? ToolResult::Text(std::move(text))
                    : ToolResult::Error(std::move(text));
}

core::Result<SceneDocument*, ToolResult> RequireScene(EditorContext& context) {
  if (SceneDocument* document = context.scene_document()) {
    return core::Result<SceneDocument*, ToolResult>::Ok(document);
  }
  return core::Result<SceneDocument*, ToolResult>::Error(ToolResult::Error(
      "No scene document is open. Open a .mscn with editor_open or create one "
      "with editor_run_command \"new-scene\"."));
}

core::Result<PrefabDocument*, ToolResult> RequirePrefab(
    EditorContext& context) {
  if (PrefabDocument* document = context.prefab_document()) {
    return core::Result<PrefabDocument*, ToolResult>::Ok(document);
  }
  return core::Result<PrefabDocument*, ToolResult>::Error(ToolResult::Error(
      "No prefab document is open. Open a .pbo with editor_open or import a "
      "model with prefab_import_model."));
}

Json EditorStateJson(EditorContext& context) {
  Json selection = Json::Array();
  for (const SelectionItem& item : context.selection().items()) {
    Json entry = Json::Object().Set("kind", SelectionKindName(item.kind));
    if (item.index >= 0) {
      entry.Set("index", item.index);
    }
    selection.Push(std::move(entry));
  }
  return Json::Object()
      .Set("mode", context.mode() == EditorMode::kPrefab ? "prefab" : "scene")
      .Set("asset_root", context.asset_root())
      .Set("prefab_document",
           DocumentJson(context.prefab_document(), context.asset_root()))
      .Set("scene_document",
           DocumentJson(context.scene_document(), context.asset_root()))
      .Set("selection", std::move(selection))
      .Set("status", context.status());
}

}  // namespace blue::editor::mcp_tools
