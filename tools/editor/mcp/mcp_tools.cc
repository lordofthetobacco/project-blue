// The editor's MCP tools: the editor itself, documents, assets, the
// viewport, playing the game, and the resources. Scene and prefab editing
// tools live in mcp_scene_tools.cc and mcp_prefab_tools.cc.
//
//   editor_get_state         mode, documents, selection, undo / redo labels
//   editor_list_commands     the text command registry (name: usage)
//   editor_run_command       runs editor command lines, returns their output
//   editor_open / editor_save / editor_undo / editor_redo / editor_set_mode
//   assets_list              files under the asset root (kind, size)
//   prefab_import_model      cooks a model into a new prefab (pending)
//   viewport_set_camera      orbit camera or frame the selection
//   viewport_screenshot      the viewport as a PNG image (pending)
//   game_play                saves the scene and starts the game on it
//   resources                blue://scene, blue://prefab, blue://log

#include "editor/mcp/mcp_tools.h"

#include <SDL3/SDL_filesystem.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glm/trigonometric.hpp>
#include <glm/vec3.hpp>

#include "core/log.h"
#include "core/path.h"
#include "core/result.h"
#include "editor/commands.h"
#include "editor/dialogs.h"
#include "editor/document.h"
#include "editor/editor_context.h"
#include "editor/mcp/mcp_args.h"
#include "editor/mcp/mcp_export.h"
#include "editor/mcp/mcp_tool_util.h"
#include "editor/mcp/png_encode.h"
#include "editor/panel.h"
#include "editor/viewport/editor_camera.h"
#include "editor/viewport/viewport_panel.h"
#include "mcp/json.h"
#include "mcp/server.h"
#include "mcp/tool.h"
#include "platform/file_system.h"
#include "render/renderer.h"

namespace blue::editor {

using mcp_tools::AddTool;
using mcp_tools::AnyProp;
using mcp_tools::CommandOutcome;
using mcp_tools::EnumProp;
using mcp_tools::Json;
using mcp_tools::ObjectSchema;
using mcp_tools::OutcomeResult;
using mcp_tools::Prop;
using mcp_tools::RunCommandLine;
using mcp_tools::ToolCall;
using mcp_tools::ToolResult;
using mcp_tools::ToolReturn;
using mcp_tools::VectorProp;

namespace {

// Frames after which a pending screenshot gives up (the viewport is not
// being rendered, e.g. the window is minimized).
constexpr uint64_t kScreenshotFrameLimit = 600;
// Cooking a large model with high-quality BC7 textures can take minutes.
constexpr std::chrono::milliseconds kImportTimeout{600000};
// Frames after which a pending import is forgotten (well past the timeout).
constexpr uint64_t kImportFrameLimit = 200000;
constexpr int kDefaultScreenshotWidth = 1024;
constexpr size_t kMaxListedAssets = 2000;
constexpr size_t kMaxLogLines = 200;

// An asset file found by assets_list.
struct AssetEntry {
  std::string path;  // relative to the asset root, '/' separated
  const char* kind = "";
  uint64_t size = 0;
};

const char* AssetKind(const std::string& path) {
  if (MatchesFileKind(path, FileKind::kPrefab)) {
    return "prefab";
  }
  if (MatchesFileKind(path, FileKind::kScene)) {
    return "scene";
  }
  if (MatchesFileKind(path, FileKind::kModel)) {
    return "model";
  }
  if (MatchesFileKind(path, FileKind::kHdr)) {
    return "hdri";
  }
  if (MatchesFileKind(path, FileKind::kImage)) {
    return "image";
  }
  return nullptr;
}

SDL_EnumerationResult CollectName(void* user_data, const char* /*dirname*/,
                                  const char* name) {
  static_cast<std::vector<std::string>*>(user_data)->emplace_back(name);
  return SDL_ENUM_CONTINUE;
}

// Lists the editor's file kinds under `directory` (relative path `prefix`),
// depth first, up to kMaxListedAssets entries.
void ListAssets(const std::string& directory, const std::string& prefix,
                bool recursive, std::vector<AssetEntry>& out) {
  std::vector<std::string> names;
  if (!SDL_EnumerateDirectory(directory.c_str(), &CollectName, &names)) {
    return;
  }
  std::sort(names.begin(), names.end());
  for (const std::string& name : names) {
    if (out.size() >= kMaxListedAssets) {
      return;
    }
    const std::string path = core::JoinPath(directory, name);
    const std::string relative = prefix.empty() ? name : prefix + "/" + name;
    SDL_PathInfo info{};
    if (!SDL_GetPathInfo(path.c_str(), &info)) {
      continue;
    }
    if (info.type == SDL_PATHTYPE_DIRECTORY) {
      if (recursive) {
        ListAssets(path, relative, recursive, out);
      }
    } else if (const char* kind = AssetKind(name)) {
      out.push_back({.path = relative, .kind = kind, .size = info.size});
    }
  }
}

// Returns the mode a file opens in, or nullopt for other files.
std::optional<EditorMode> ModeOfFile(const std::string& path) {
  if (MatchesFileKind(path, FileKind::kPrefab)) {
    return EditorMode::kPrefab;
  }
  if (MatchesFileKind(path, FileKind::kScene)) {
    return EditorMode::kScene;
  }
  return std::nullopt;
}

bool DocumentDirty(EditorContext& context, EditorMode mode) {
  if (mode == EditorMode::kPrefab) {
    return context.prefab_document() != nullptr &&
           context.prefab_document()->dirty();
  }
  return context.scene_document() != nullptr &&
         context.scene_document()->dirty();
}

std::string QuotedOrEmpty(std::string_view word) {
  core::Result<std::string, std::string> quoted = QuoteCommandWord(word);
  return quoted.ok() ? quoted.value() : std::string();
}

}  // namespace

std::string EditorMcpInstructions() {
  return "blue-editor is the level and prefab editor of a game engine. Two "
         "documents can be open: a prefab (.pbo: meshes, materials, "
         "textures, animation clips, sockets, lights, collider) edited in "
         "Prefab mode, and a scene (.mscn: placed prefab instances, sun, "
         "environment, point lights, player start) edited in Scene mode.\n"
         "Start with editor_get_state; read a document with scene_get or "
         "prefab_get before editing it, and find files with assets_list "
         "(paths may be relative to the asset root). Every edit tool is one "
         "undo step (editor_undo reverts it); nothing is written to disk "
         "until editor_save. Look at the result with viewport_screenshot "
         "(set the view with viewport_set_camera first). Scene instances are "
         "addressed by their numeric id from scene_get. Anything without a "
         "dedicated tool can be done with editor_run_command using the "
         "commands listed by editor_list_commands. The user sees every "
         "change live in the editor window.";
}

EditorTools::EditorTools(EditorContext& context, mcp::Server& server)
    : context_(context), server_(server) {
  RegisterEditorTools();
  RegisterDocumentTools();
  mcp_tools::RegisterSceneTools(server_, context_);
  mcp_tools::RegisterPrefabTools(server_, context_);
  RegisterViewportTools();
  RegisterResources();
}

EditorTools::~EditorTools() = default;

void EditorTools::RegisterEditorTools() {
  EditorContext& context = context_;
  AddTool(server_, context,
          {.name = "editor_get_state",
           .title = "Get editor state",
           .description =
               "Returns the editor's mode (prefab or scene), the open prefab "
               "and scene documents (path, dirty flag, next undo / redo "
               "label), the selection, the asset root directory and the "
               "status bar text.",
           .input_schema = ObjectSchema(Json::Object()),
           .read_only = true},
          [&context](const ToolCall&) -> ToolReturn {
            return ToolResult::Structured(mcp_tools::EditorStateJson(context));
          });

  AddTool(server_, context,
          {.name = "editor_list_commands",
           .title = "List editor commands",
           .description =
               "Returns every editor text command as 'name: usage'. These "
               "are what editor_run_command runs (the same commands as the "
               "editor's menus and --exec scripts).",
           .input_schema = ObjectSchema(Json::Object()),
           .read_only = true},
          [&context](const ToolCall&) -> ToolReturn {
            std::string text;
            for (const std::string& line : context.commands().Help()) {
              text += line + "\n";
            }
            return ToolResult::Text(std::move(text));
          });

  AddTool(
      server_, context,
      {.name = "editor_run_command",
       .title = "Run editor commands",
       .description =
           "Runs editor command lines (see editor_list_commands), in order, "
           "and returns for each whether it succeeded, its error and the "
           "status messages it produced. A line may hold several commands "
           "separated by ';' (a line stops at its first failure). Words with "
           "spaces go in double quotes. 'wait' is ignored. Commands without "
           "a path argument may open a file dialog for the user.",
       .input_schema = ObjectSchema(
           Json::Object().Set(
               "commands",
               AnyProp("One command line (string) or an array of them, e.g. "
                       "[\"scene-select #3\", \"scene-move 0 1 0\"].")),
           {"commands"})},
      [&context](const ToolCall& call) -> ToolReturn {
        core::Result<std::vector<std::string>, std::string> lines =
            GetStringListArg(call.arguments, "commands");
        if (!lines.ok() || lines.value().empty()) {
          return ToolResult::Error(
              "commands must be a command line or a non-empty array of them");
        }
        Json results = Json::Array();
        bool all_ok = true;
        for (const std::string& line : lines.value()) {
          const CommandOutcome outcome = RunCommandLine(context, line);
          all_ok = all_ok && outcome.ok;
          Json messages = Json::Array();
          for (const std::string& message : outcome.messages) {
            messages.Push(Json::String(message));
          }
          Json entry =
              Json::Object().Set("command", line).Set("ok", outcome.ok);
          if (!outcome.ok) {
            entry.Set("error", outcome.error);
          }
          entry.Set("output", std::move(messages));
          results.Push(std::move(entry));
        }
        ToolResult result = ToolResult::Structured(
            Json::Object().Set("results", std::move(results)));
        result.is_error = !all_ok;
        return result;
      });

  AddTool(server_, context,
          {.name = "game_play",
           .title = "Play the scene",
           .description =
               "Saves the open scene (it must have a path) and starts the "
               "game executable on it in its own window. Returns at once; "
               "the game runs independently of the editor.",
           .input_schema = ObjectSchema(Json::Object())},
          [&context](const ToolCall&) -> ToolReturn {
            return OutcomeResult(RunCommandLine(context, "play"),
                                 "The game was started.");
          });
}

void EditorTools::RegisterDocumentTools() {
  EditorContext& context = context_;
  AddTool(
      server_, context,
      {.name = "editor_open",
       .title = "Open a document",
       .description =
           "Opens a prefab (.pbo, switches to Prefab mode) or a scene (.mscn, "
           "switches to Scene mode), replacing the open document of that "
           "kind. Fails if that document has unsaved changes unless "
           "discard_unsaved is true. Models (.gltf, .glb, .fbx, .obj) are "
           "imported with prefab_import_model instead.",
       .input_schema = ObjectSchema(
           Json::Object()
               .Set("path", Prop("string",
                                 "File to open; relative paths are taken "
                                 "relative to the asset root."))
               .Set("discard_unsaved",
                    Prop("boolean",
                         "Drop unsaved changes of the document being "
                         "replaced (default false).")),
           {"path"})},
      [&context](const ToolCall& call) -> ToolReturn {
        const std::string path = mcp_tools::ResolveAssetPath(
            context, *call.arguments.GetString("path"));
        const std::optional<EditorMode> mode = ModeOfFile(path);
        if (!mode) {
          return ToolResult::Error(
              "Only .pbo and .mscn files can be opened (use "
              "prefab_import_model for models).");
        }
        if (!platform::IsRegularFile(path)) {
          return ToolResult::Error(std::format("{} does not exist", path));
        }
        const bool discard =
            call.arguments.GetBool("discard_unsaved").value_or(false);
        if (DocumentDirty(context, *mode) && !discard) {
          return ToolResult::Error(
              "The open document has unsaved changes: save it with "
              "editor_save, or pass discard_unsaved: true.");
        }
        const std::string quoted = QuotedOrEmpty(path);
        if (quoted.empty()) {
          return ToolResult::Error("The path cannot contain '\"'.");
        }
        const CommandOutcome outcome =
            RunCommandLine(context, "open " + quoted);
        if (context.pending_replace()) {
          context.ResolvePendingReplace(EditorContext::ReplaceChoice::kDiscard);
        }
        return OutcomeResult(
            outcome, std::format("Opened {}", context.DisplayPath(path)));
      });

  AddTool(
      server_, context,
      {.name = "editor_save",
       .title = "Save the active document",
       .description =
           "Saves the document of the active mode to its path, or to `path` "
           "(Save As; the document takes that path). An untitled document "
           "needs a path. Prefabs are validated first (see "
           "prefab_validate).",
       .input_schema = ObjectSchema(Json::Object().Set(
           "path", Prop("string",
                        "Optional target file (.pbo for prefabs, .mscn for "
                        "scenes); relative to the asset root.")))},
      [&context](const ToolCall& call) -> ToolReturn {
        std::string path;
        if (const std::string* given = call.arguments.GetString("path")) {
          path = mcp_tools::ResolveAssetPath(context, *given);
        } else {
          const bool prefab = context.mode() == EditorMode::kPrefab;
          if (prefab ? context.prefab_document() == nullptr
                     : context.scene_document() == nullptr) {
            return ToolResult::Error("No document is open in this mode.");
          }
          path = prefab ? context.prefab_document()->path()
                        : context.scene_document()->path();
          if (path.empty()) {
            return ToolResult::Error(
                "The document is untitled: give a path to save it to.");
          }
        }
        core::Result<void, std::string> saved = context.SaveActiveTo(path);
        if (!saved.ok()) {
          return ToolResult::Error(saved.error());
        }
        return ToolResult::Text(
            std::format("Saved {}", context.DisplayPath(path)));
      });

  const auto undo_redo = [&context](bool undo) {
    return [&context, undo](const ToolCall& call) -> ToolReturn {
      const int64_t steps = std::clamp<int64_t>(
          call.arguments.GetInteger("steps").value_or(1), 1, 100);
      int64_t done = 0;
      while (done < steps && (undo ? context.Undo() : context.Redo())) {
        ++done;
      }
      if (done == 0) {
        return ToolResult::Error(undo ? "Nothing to undo."
                                      : "Nothing to redo.");
      }
      Json state = mcp_tools::EditorStateJson(context);
      return ToolResult::Structured(
          Json::Object()
              .Set(undo ? "undone_steps" : "redone_steps", done)
              .Set("state", std::move(state)));
    };
  };
  const Json steps_schema = ObjectSchema(Json::Object().Set(
      "steps", Prop("integer", "How many steps (default 1, at most 100).")));
  AddTool(server_, context,
          {.name = "editor_undo",
           .title = "Undo",
           .description =
               "Undoes the last edit(s) of the active document (each edit "
               "tool call is one step). Returns the editor state with the "
               "next undo / redo labels.",
           .input_schema = steps_schema},
          undo_redo(true));
  AddTool(server_, context,
          {.name = "editor_redo",
           .title = "Redo",
           .description = "Redoes undone edit(s) of the active document.",
           .input_schema = steps_schema},
          undo_redo(false));

  AddTool(server_, context,
          {.name = "editor_set_mode",
           .title = "Switch mode",
           .description =
               "Switches between Prefab mode (edits the prefab document) "
               "and Scene mode (edits the scene document). The viewport, "
               "undo / redo and editor_save act on the active mode.",
           .input_schema = ObjectSchema(
               Json::Object().Set("mode",
                                  EnumProp({"prefab", "scene"}, "The mode.")),
               {"mode"})},
          [&context](const ToolCall& call) -> ToolReturn {
            const bool prefab = *call.arguments.GetString("mode") == "prefab";
            context.SetMode(prefab ? EditorMode::kPrefab : EditorMode::kScene);
            return ToolResult::Text(prefab ? "Prefab mode" : "Scene mode");
          });

  AddTool(
      server_, context,
      {.name = "assets_list",
       .title = "List assets",
       .description =
           "Lists the files the editor works with under the asset root (or "
           "a subdirectory of it): prefabs (.pbo), scenes (.mscn), models, "
           "images and HDRIs, with their size in bytes. Paths are relative "
           "to the asset root and can be passed to the other tools as they "
           "are.",
       .input_schema = ObjectSchema(
           Json::Object()
               .Set("directory",
                    Prop("string",
                         "Subdirectory of the asset root (default: the "
                         "root)."))
               .Set("recursive",
                    Prop("boolean", "Include subdirectories (default true)."))
               .Set("kind",
                    EnumProp({"prefab", "scene", "model", "image", "hdri"},
                             "Only files of this kind."))),
       .read_only = true},
      [&context](const ToolCall& call) -> ToolReturn {
        std::string prefix;
        if (const std::string* directory =
                call.arguments.GetString("directory")) {
          prefix = core::NormalizePath(*directory);
          while (!prefix.empty() && prefix.back() == '/') {
            prefix.pop_back();
          }
          if (prefix == ".") {
            prefix.clear();
          }
          if (prefix.starts_with("..") || prefix.starts_with("/") ||
              prefix.find(':') != std::string::npos) {
            return ToolResult::Error(
                "directory must be a subdirectory of the asset root");
          }
        }
        const std::string root =
            prefix.empty() ? context.asset_root()
                           : core::JoinPath(context.asset_root(), prefix);
        std::vector<AssetEntry> entries;
        ListAssets(root, prefix,
                   call.arguments.GetBool("recursive").value_or(true), entries);
        const std::string* kind = call.arguments.GetString("kind");
        Json files = Json::Array();
        for (const AssetEntry& entry : entries) {
          if (kind != nullptr && *kind != entry.kind) {
            continue;
          }
          files.Push(Json::Object()
                         .Set("path", entry.path)
                         .Set("kind", entry.kind)
                         .Set("bytes", static_cast<int64_t>(entry.size)));
        }
        return ToolResult::Structured(
            Json::Object()
                .Set("asset_root", context.asset_root())
                .Set("truncated", entries.size() >= kMaxListedAssets)
                .Set("files", std::move(files)));
      });

  AddTool(
      server_, context,
      {.name = "prefab_import_model",
       .title = "Import a model",
       .description =
           "Cooks a model file (.gltf, .glb, .fbx, .obj) into a new, untitled "
           "prefab document (meshes, materials, BC-compressed textures, "
           "clips) and switches to Prefab mode. Cooking runs in the "
           "background and may take a while; the result arrives when it is "
           "done. Save the prefab afterwards with editor_save and a .pbo "
           "path. Fails if the open prefab has unsaved changes unless "
           "discard_unsaved is true.",
       .input_schema = ObjectSchema(
           Json::Object()
               .Set("path",
                    Prop("string", "Model file; relative to the asset root."))
               .Set("scale", Prop("number", "Uniform scale (default 1)."))
               .Set("z_up",
                    Prop("boolean", "The model is Z-up (default false: Y-up)."))
               .Set("merge_static_nodes",
                    Prop("boolean",
                         "Merge non-animated nodes into one mesh (default "
                         "false)."))
               .Set("fast_textures",
                    Prop("boolean",
                         "Faster, lower-quality texture compression (default "
                         "false)."))
               .Set("drop_nodes",
                    AnyProp("Node name or array of node names to leave out."))
               .Set("discard_unsaved",
                    Prop("boolean",
                         "Drop unsaved changes of the open prefab (default "
                         "false).")),
           {"path"}),
       .timeout = kImportTimeout},
      [this](const ToolCall& call) -> ToolReturn {
        const Json& args = call.arguments;
        const std::string path =
            mcp_tools::ResolveAssetPath(context_, *args.GetString("path"));
        if (!MatchesFileKind(path, FileKind::kModel)) {
          return ToolResult::Error(
              "path must be a .gltf, .glb, .fbx or .obj model");
        }
        if (!platform::IsRegularFile(path)) {
          return ToolResult::Error(std::format("{} does not exist", path));
        }
        const bool discard = args.GetBool("discard_unsaved").value_or(false);
        if (DocumentDirty(context_, EditorMode::kPrefab) && !discard) {
          return ToolResult::Error(
              "The open prefab has unsaved changes: save it with editor_save, "
              "or pass discard_unsaved: true.");
        }
        std::vector<std::string> words{"prefab-import", path};
        core::Result<std::optional<float>, std::string> scale =
            GetFloatArg(args, "scale", 1.0e-6f, 1.0e6f);
        if (!scale.ok()) {
          return ToolResult::Error("scale must be > 0");
        }
        if (scale.value()) {
          words.push_back("scale");
          words.push_back(std::format("{}", *scale.value()));
        }
        if (args.GetBool("z_up").value_or(false)) {
          words.push_back("zup");
        }
        if (args.GetBool("merge_static_nodes").value_or(false)) {
          words.push_back("merge");
        }
        if (args.GetBool("fast_textures").value_or(false)) {
          words.push_back("fast");
        }
        core::Result<std::vector<std::string>, std::string> drop =
            GetStringListArg(args, "drop_nodes");
        if (!drop.ok()) {
          return ToolResult::Error(drop.error());
        }
        for (const std::string& node : drop.value()) {
          words.push_back("drop");
          words.push_back(node);
        }
        core::Result<std::string, std::string> line = BuildCommand(words);
        if (!line.ok()) {
          return ToolResult::Error(line.error());
        }
        const uint64_t first = context_.next_message_serial();
        const CommandOutcome outcome = RunCommandLine(context_, line.value());
        if (!outcome.ok) {
          return OutcomeResult(outcome, "");
        }
        imports_.push_back({.call_id = call.id,
                            .source = path,
                            .discard_unsaved = discard,
                            .first_message = first,
                            .requested_frame = context_.frame_count()});
        return std::nullopt;
      });
}

void EditorTools::RegisterViewportTools() {
  EditorContext& context = context_;
  AddTool(
      server_, context,
      {.name = "viewport_set_camera",
       .title = "Set the viewport camera",
       .description =
           "Moves the viewport's orbit camera, which looks at `target` from "
           "`distance` metres along yaw / pitch. Y is up; yaw 0 looks along "
           "-Z; negative pitch looks down. Give any of yaw_deg, pitch_deg, "
           "distance, target, or eye + target, or frame_selection: true to "
           "frame the selection (everything when nothing is selected). "
           "Returns the resulting camera.",
       .input_schema = ObjectSchema(
           Json::Object()
               .Set("yaw_deg", Prop("number", "Yaw about +Y in degrees."))
               .Set("pitch_deg", Prop("number", "Pitch in degrees (-89..89)."))
               .Set("distance",
                    Prop("number", "Distance from the target in metres."))
               .Set("target", VectorProp(3, "Point looked at [x, y, z]."))
               .Set("eye", VectorProp(3,
                                      "Camera position [x, y, z]; looks at "
                                      "target (or the current target)."))
               .Set("frame_selection",
                    Prop("boolean", "Frame the selection first.")))},
      [&context](const ToolCall& call) -> ToolReturn {
        const Json& args = call.arguments;
        if (args.GetBool("frame_selection").value_or(false)) {
          const CommandOutcome framed = RunCommandLine(
              context, context.mode() == EditorMode::kScene ? "scene-frame"
                                                            : "view frame");
          if (!framed.ok) {
            return OutcomeResult(framed, "");
          }
        }
        auto target = GetVec3Arg(args, "target");
        auto eye = GetVec3Arg(args, "eye");
        auto yaw = GetFloatArg(args, "yaw_deg", -1.0e6f, 1.0e6f);
        auto pitch = GetFloatArg(args, "pitch_deg", -89.0f, 89.0f);
        auto distance =
            GetFloatArg(args, "distance", EditorCamera::kMinDistance,
                        EditorCamera::kMaxDistance);
        for (const std::string* error :
             {target.ok() ? nullptr : &target.error(),
              eye.ok() ? nullptr : &eye.error(),
              yaw.ok() ? nullptr : &yaw.error(),
              pitch.ok() ? nullptr : &pitch.error(),
              distance.ok() ? nullptr : &distance.error()}) {
          if (error != nullptr) {
            return ToolResult::Error(*error);
          }
        }
        EditorCamera& camera = context.viewport().camera();
        if (eye.value()) {
          camera.LookAt(*eye.value(), target.value().value_or(camera.target()));
        } else {
          if (yaw.value() || pitch.value()) {
            camera.SetOrientation(
                yaw.value() ? glm::radians(*yaw.value()) : camera.yaw(),
                pitch.value() ? glm::radians(*pitch.value()) : camera.pitch());
          }
          if (target.value() || distance.value()) {
            const glm::vec3 new_target =
                target.value().value_or(camera.target());
            const float new_distance =
                distance.value().value_or(camera.distance());
            camera.LookAt(new_target - camera.forward() * new_distance,
                          new_target);
          }
        }
        return ToolResult::Structured(
            Json::Object()
                .Set("target", Vec3Json(camera.target()))
                .Set("eye", Vec3Json(camera.position()))
                .Set("distance", static_cast<double>(camera.distance()))
                .Set("yaw_deg", static_cast<double>(glm::degrees(camera.yaw())))
                .Set("pitch_deg",
                     static_cast<double>(glm::degrees(camera.pitch()))));
      });

  AddTool(
      server_, context,
      {.name = "viewport_screenshot",
       .title = "Screenshot the viewport",
       .description =
           "Returns what the editor viewport shows (the rendered scene or "
           "prefab with grid and gizmo overlays, without the editor UI) "
           "as a PNG image, scaled down to max_width. Edits made before "
           "this call are visible in it.",
       .input_schema = ObjectSchema(Json::Object().Set(
           "max_width", Prop("integer",
                             "Maximum width in pixels (default 1024; 0 = full "
                             "size)."))),
       .read_only = true},
      [this](const ToolCall& call) -> ToolReturn {
        const render::ViewportImage& viewport = context_.renderer().viewport();
        if (viewport.generation == 0 || viewport.extent.width <= 1 ||
            viewport.extent.height <= 1) {
          return ToolResult::Error(
              "The viewport panel is not visible; show it with "
              "editor_run_command \"panel Viewport show\".");
        }
        const int64_t max_width = call.arguments.GetInteger("max_width")
                                      .value_or(kDefaultScreenshotWidth);
        screenshots_.push_back(
            {.call_id = call.id,
             .ticket = context_.renderer().RequestViewportReadback(),
             .max_width =
                 static_cast<int>(std::clamp<int64_t>(max_width, 0, 16384)),
             .requested_frame = context_.frame_count()});
        return std::nullopt;
      });
}

void EditorTools::RegisterResources() {
  EditorContext& context = context_;
  server_.RegisterResource(
      {.uri = "blue://scene",
       .name = "scene",
       .title = "Open scene",
       .description = "The open scene document as JSON (same as scene_get).",
       .mime_type = "application/json",
       .handler = [&context](std::string_view)
           -> core::Result<mcp::ResourceContents, std::string> {
         using ResourceResult =
             core::Result<mcp::ResourceContents, std::string>;
         const SceneDocument* scene = context.scene_document();
         if (scene == nullptr) {
           return ResourceResult::Error("no scene document is open");
         }
         return ResourceResult::Ok(
             {.mime_type = "application/json",
              .text = SceneToJson(scene->state(), context.asset_root())
                          .DumpPretty()});
       }});
  server_.RegisterResource(
      {.uri = "blue://prefab",
       .name = "prefab",
       .title = "Open prefab",
       .description = "The open prefab document as JSON (same as prefab_get).",
       .mime_type = "application/json",
       .handler = [&context](std::string_view)
           -> core::Result<mcp::ResourceContents, std::string> {
         using ResourceResult =
             core::Result<mcp::ResourceContents, std::string>;
         const PrefabDocument* prefab = context.prefab_document();
         if (prefab == nullptr) {
           return ResourceResult::Error("no prefab document is open");
         }
         return ResourceResult::Ok(
             {.mime_type = "application/json",
              .text = PrefabToJson(prefab->state(), context.asset_root())
                          .DumpPretty()});
       }});
  server_.RegisterResource(
      {.uri = "blue://log",
       .name = "log",
       .title = "Editor log",
       .description =
           "The recent status and error messages of the editor, oldest "
           "first.",
       .mime_type = "text/plain",
       .handler = [&context](std::string_view)
           -> core::Result<mcp::ResourceContents, std::string> {
         const auto& messages = context.messages();
         const size_t skip = messages.size() > kMaxLogLines
                                 ? messages.size() - kMaxLogLines
                                 : 0;
         std::string text;
         for (size_t i = skip; i < messages.size(); ++i) {
           text += messages[i].error ? "error: " : "";
           text += messages[i].text + "\n";
         }
         return core::Result<mcp::ResourceContents, std::string>::Ok(
             {.mime_type = "text/plain", .text = std::move(text)});
       }});
}

void EditorTools::Update() {
  if (!screenshots_.empty()) {
    UpdateScreenshots();
  }
  if (!imports_.empty()) {
    UpdateImports();
  }
}

void EditorTools::UpdateScreenshots() {
  std::erase_if(screenshots_, [this](const PendingScreenshot& shot) {
    render::ViewportPixels pixels;
    const render::ReadbackStatus status =
        context_.renderer().TakeViewportReadback(shot.ticket, pixels);
    if (status == render::ReadbackStatus::kPending) {
      if (context_.frame_count() - shot.requested_frame <
          kScreenshotFrameLimit) {
        return false;
      }
      server_.CompleteCall(shot.call_id,
                           ToolResult::Error("The viewport was not rendered "
                                             "(is the window minimized?)."));
      return true;
    }
    if (status == render::ReadbackStatus::kUnknown) {
      server_.CompleteCall(shot.call_id,
                           ToolResult::Error("The screenshot was lost."));
      return true;
    }
    const auto start = std::chrono::steady_clock::now();
    RgbaImage image = FitWidth({.width = pixels.width,
                                .height = pixels.height,
                                .pixels = std::move(pixels.rgba)},
                               shot.max_width);
    const std::vector<std::byte> png = EncodePng(image);
    if (png.empty()) {
      server_.CompleteCall(shot.call_id,
                           ToolResult::Error("PNG encoding failed."));
      return true;
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count();
    core::log::Info("MCP screenshot {} x {}: {} KiB PNG in {} ms", image.width,
                    image.height, png.size() / 1024, ms);
    ToolResult result = ToolResult::Text(std::format(
        "Viewport {} x {} ({} mode).", image.width, image.height,
        context_.mode() == EditorMode::kPrefab ? "Prefab" : "Scene"));
    result.AddImage(mcp::EncodeBase64(png.data(), png.size()), "image/png");
    server_.CompleteCall(shot.call_id, std::move(result));
    return true;
  });
}

void EditorTools::UpdateImports() {
  std::erase_if(imports_, [this](const PendingImport& import) {
    // A dirty prefab document asks before it is replaced: answer for the
    // client (dirty documents were refused unless discard_unsaved).
    if (const auto& pending = context_.pending_replace();
        pending && pending->mode == EditorMode::kPrefab) {
      if (!import.discard_unsaved) {
        context_.ResolvePendingReplace(EditorContext::ReplaceChoice::kCancel);
        server_.CompleteCall(
            import.call_id,
            ToolResult::Error("The prefab document got unsaved changes "
                              "during the import; the import was dropped."));
        return true;
      }
      context_.ResolvePendingReplace(EditorContext::ReplaceChoice::kDiscard);
    }
    const std::string imported =
        std::format("Imported {}", core::FileName(import.source));
    for (const EditorContext::Message& message : context_.messages()) {
      if (message.serial < import.first_message) {
        continue;
      }
      if (message.error && message.text.starts_with("Import model")) {
        server_.CompleteCall(import.call_id, ToolResult::Error(message.text));
        return true;
      }
      if (!message.error && message.text == imported &&
          context_.prefab_document() != nullptr) {
        ToolResult result = ToolResult::Structured(PrefabToJson(
            context_.prefab_document()->state(), context_.asset_root()));
        result.content.insert(
            result.content.begin(),
            {.kind = mcp::ContentItem::Kind::kText,
             .text = imported + " into a new untitled prefab document (Prefab "
                                "mode). Save it with editor_save and a .pbo "
                                "path."});
        server_.CompleteCall(import.call_id, std::move(result));
        return true;
      }
    }
    // Still cooking. The server times the call out; forget it much later.
    return context_.frame_count() - import.requested_frame > kImportFrameLimit;
  });
}

}  // namespace blue::editor
