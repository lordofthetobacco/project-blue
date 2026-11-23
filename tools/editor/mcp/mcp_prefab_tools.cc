// The editor's MCP tools for the prefab document (Prefab mode): thin
// wrappers over prefab_edit.h, the cook functions and the prefab text
// commands, each one undo step.
//
//   prefab_get, prefab_set_material, prefab_add_socket, prefab_set_socket,
//   prefab_set_tag, prefab_set_collider, prefab_clip_edit, prefab_validate

#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <glm/gtc/quaternion.hpp>
#include <glm/trigonometric.hpp>

#include "cook/collision_cook.h"
#include "core/result.h"
#include "editor/document.h"
#include "editor/editor_context.h"
#include "editor/mcp/mcp_args.h"
#include "editor/mcp/mcp_export.h"
#include "editor/mcp/mcp_tool_util.h"
#include "editor/panel.h"
#include "editor/prefab_mode/prefab_edit.h"
#include "editor/prefab_mode/prefab_mode_state.h"
#include "editor/selection.h"
#include "editor/undo.h"
#include "mcp/json.h"
#include "mcp/server.h"
#include "prefab/prefab.h"

namespace blue::editor::mcp_tools {
namespace {

using PrefabResult = core::Result<PrefabDocument*, ToolResult>;
using IndexResult = core::Result<int, std::string>;

// The prefab document, switching to Prefab mode so the user sees the edit.
PrefabResult PrefabForEdit(EditorContext& context) {
  PrefabResult prefab = RequirePrefab(context);
  if (prefab.ok() && context.mode() != EditorMode::kPrefab) {
    context.SetMode(EditorMode::kPrefab);
  }
  return prefab;
}

// Resolves an item given by name (string) or index (integer) among `count`
// items whose names `name_of(i)` returns. `what` names the kind in errors.
template <typename NameOf>
IndexResult ResolveIndex(const Json* value, size_t count, NameOf name_of,
                         std::string_view what) {
  if (value != nullptr && value->is_string()) {
    for (size_t i = 0; i < count; ++i) {
      if (name_of(i) == value->string_value()) {
        return IndexResult::Ok(static_cast<int>(i));
      }
    }
    return IndexResult::Error(std::format("no {} named '{}' (see prefab_get)",
                                          what, value->string_value()));
  }
  if (value != nullptr && value->is_integer() && value->integer_value() >= 0 &&
      static_cast<size_t>(value->integer_value()) < count) {
    return IndexResult::Ok(static_cast<int>(value->integer_value()));
  }
  return IndexResult::Error(std::format(
      "{} must be a {} name or index (see prefab_get)", what, what));
}

// Resolves a node by name or index; -1 (or absent with `allow_root`) is the
// prefab root.
IndexResult ResolveNode(const prefab::Prefab& prefab, const Json* value) {
  if (value == nullptr ||
      (value->is_integer() && value->integer_value() == -1)) {
    return IndexResult::Ok(-1);
  }
  return ResolveIndex(
      value, prefab.hierarchy.nodes.size(),
      [&](size_t i) -> const std::string& {
        return prefab.hierarchy.nodes[i].name;
      },
      "node");
}

// Applies "position" and "rotation_deg" of `args` to `local`.
ArgsStatus ApplyLocalArgs(const Json& args, animation::Transform& local) {
  auto position = GetVec3Arg(args, "position");
  auto rotation = GetVec3Arg(args, "rotation_deg");
  if (!position.ok() || !rotation.ok()) {
    return ArgsStatus::Error(!position.ok() ? position.error()
                                            : rotation.error());
  }
  if (position.value()) {
    local.translation = *position.value();
  }
  if (rotation.value()) {
    local.rotation = glm::quat(glm::radians(*rotation.value()));
  }
  return ArgsStatus::Ok();
}

Json SocketJson(const prefab::Prefab& prefab, int index) {
  const prefab::Socket& socket = prefab.sockets[static_cast<size_t>(index)];
  return Json::Object()
      .Set("index", index)
      .Set("name", socket.name)
      .Set("node", socket.node)
      .Set("local", TransformJson(socket.local));
}

Json SocketSchema(bool for_set) {
  Json properties = Json::Object();
  if (for_set) {
    properties.Set("socket", AnyProp("Socket name or index."));
  }
  properties.Set("name", Prop("string", for_set ? "New name." : "Socket name."))
      .Set("node", AnyProp("Parent node name or index (-1: the root)."))
      .Set("position", VectorProp(3, "Local position [x, y, z] in metres."))
      .Set("rotation_deg",
           VectorProp(3, "Local rotation, Euler [x, y, z] in degrees."));
  return for_set ? ObjectSchema(std::move(properties), {"socket"})
                 : ObjectSchema(std::move(properties), {"name"});
}

}  // namespace

void RegisterPrefabTools(mcp::Server& server, EditorContext& context) {
  EditorContext* ctx = &context;

  AddTool(server, context,
          {.name = "prefab_get",
           .title = "Get the prefab",
           .description =
               "Returns the open prefab as JSON: summary (counts and byte "
               "sizes), bounds, nodes (index, name, parent, rest transform), "
               "meshes (node, vertices, triangles, submeshes with material "
               "index), materials (every parameter and the texture per "
               "slot), textures (format, size, mips, bytes), clips "
               "(duration, loop, events), sockets, lights, children, physics "
               "(collider and body) and the source model.",
           .input_schema = ObjectSchema(Json::Object()),
           .read_only = true},
          [ctx](const ToolCall&) -> ToolReturn {
            PrefabResult prefab = RequirePrefab(*ctx);
            if (!prefab.ok()) {
              return prefab.error();
            }
            return ToolResult::Structured(
                PrefabToJson(prefab.value()->state(), ctx->asset_root()));
          });

  AddTool(
      server, context,
      {.name = "prefab_set_material",
       .title = "Set material parameters",
       .description =
           "Sets parameters of one prefab material (by name or index): "
           "base_color [r,g,b,a] linear 0..1, metallic, roughness 0..1, "
           "emissive [r,g,b], emissive_strength, normal_scale, "
           "occlusion_strength 0..1, uv_scale [u,v], alpha_mode "
           "(opaque|mask), alpha_cutoff 0..1, double_sided. Returns the "
           "material.",
       .input_schema = ObjectSchema(
           Json::Object()
               .Set("material", AnyProp("Material name or index."))
               .Set("base_color", VectorProp(4, "Linear RGBA, 0..1."))
               .Set("metallic", Prop("number", "0..1"))
               .Set("roughness", Prop("number", "0..1"))
               .Set("emissive", VectorProp(3, "Linear RGB, >= 0."))
               .Set("emissive_strength", Prop("number", ">= 0"))
               .Set("normal_scale", Prop("number", ">= 0"))
               .Set("occlusion_strength", Prop("number", "0..1"))
               .Set("uv_scale", VectorProp(2, "[u, v], > 0."))
               .Set("alpha_mode", EnumProp({"opaque", "mask"}, "Alpha mode."))
               .Set("alpha_cutoff", Prop("number", "0..1"))
               .Set("double_sided", Prop("boolean", "Draw back faces.")),
           {"material"})},
      [ctx](const ToolCall& call) -> ToolReturn {
        PrefabResult prefab = PrefabForEdit(*ctx);
        if (!prefab.ok()) {
          return prefab.error();
        }
        PrefabDocument& doc = *prefab.value();
        const prefab::Prefab& state = doc.state();
        IndexResult index = ResolveIndex(
            call.arguments.Find("material"), state.materials.size(),
            [&](size_t i) -> const std::string& {
              return state.materials[i].name;
            },
            "material");
        if (!index.ok()) {
          return ToolResult::Error(index.error());
        }
        const auto i = static_cast<size_t>(index.value());
        prefab::Material material = state.materials[i];
        core::Result<int, std::string> applied =
            ApplyMaterialArgs(call.arguments, material);
        if (!applied.ok()) {
          return ToolResult::Error(applied.error());
        }
        doc.Edit("Edit material", ChangeKind::kParameters,
                 [&](prefab::Prefab& p) { p.materials[i] = material; });
        doc.selection().Set(
            {.kind = SelectionKind::kMaterial, .index = index.value()});
        const Json json = PrefabToJson(doc.state(), ctx->asset_root());
        Json result = json.GetArray("materials")->array_items()[i];
        result.Set("undo", UndoNote(doc));
        return ToolResult::Structured(std::move(result));
      });

  AddTool(
      server, context,
      {.name = "prefab_add_socket",
       .title = "Add a socket",
       .description =
           "Adds a named attachment point (socket) to the prefab under a "
           "node (default: the root) at a local position / rotation. "
           "The name is made unique. Returns the socket.",
       .input_schema = SocketSchema(false)},
      [ctx](const ToolCall& call) -> ToolReturn {
        PrefabResult prefab = PrefabForEdit(*ctx);
        if (!prefab.ok()) {
          return prefab.error();
        }
        PrefabDocument& doc = *prefab.value();
        const Json& args = call.arguments;
        IndexResult node = ResolveNode(doc.state(), args.Find("node"));
        animation::Transform local;
        ArgsStatus placed = ApplyLocalArgs(args, local);
        const std::string name = *args.GetString("name");
        if (!node.ok() || !placed.ok() || name.empty()) {
          return ToolResult::Error(!node.ok()     ? node.error()
                                   : !placed.ok() ? placed.error()
                                                  : "name is empty");
        }
        int added = -1;
        doc.Edit("Add socket", ChangeKind::kParameters, [&](prefab::Prefab& p) {
          added = AddSocket(p, node.value(), name);
          p.sockets[static_cast<size_t>(added)].local = local;
        });
        doc.selection().Set({.kind = SelectionKind::kSocket, .index = added});
        Json result = SocketJson(doc.state(), added);
        result.Set("undo", UndoNote(doc));
        return ToolResult::Structured(std::move(result));
      });

  AddTool(
      server, context,
      {.name = "prefab_set_tag",
       .title = "Set or remove a tag",
       .description =
           "Sets prefab tag `key` to `value` (adds it or changes its value), "
           "or removes it with remove = true. Keys are 1-64 of [a-z0-9_.], "
           "values at most 256 bytes. The tag weapon = <type> (glock17, "
           "m16) makes the prefab a weapon the game can pick up. Returns "
           "every tag.",
       .input_schema = ObjectSchema(
           Json::Object()
               .Set("key", Prop("string", "Tag key."))
               .Set("value", Prop("string", "New value (set)."))
               .Set("remove", Prop("boolean", "Remove the tag instead.")),
           {"key"})},
      [ctx](const ToolCall& call) -> ToolReturn {
        PrefabResult prefab = PrefabForEdit(*ctx);
        if (!prefab.ok()) {
          return prefab.error();
        }
        PrefabDocument& doc = *prefab.value();
        const Json& args = call.arguments;
        const std::string key = *args.GetString("key");
        const std::string* value = args.GetString("value");
        const Json* remove = args.Find("remove");
        const bool removing =
            remove != nullptr && remove->is_bool() && remove->bool_value();
        if (!removing && value == nullptr) {
          return ToolResult::Error("give a value, or remove = true");
        }
        core::Result<void, std::string> edited =
            TryEditQuiet(doc, removing ? "Remove tag" : "Set tag",
                         ChangeKind::kParameters, [&](prefab::Prefab& p) {
                           return removing ? RemovePrefabTag(p, key)
                                           : SetPrefabTag(p, key, *value);
                         });
        if (!edited.ok()) {
          return ToolResult::Error(edited.error());
        }
        Json tags = Json::Object();
        for (const prefab::Tag& tag : doc.state().tags) {
          tags.Set(tag.key, tag.value);
        }
        Json result = Json::Object().Set("tags", std::move(tags));
        result.Set("undo", UndoNote(doc));
        return ToolResult::Structured(std::move(result));
      });

  AddTool(
      server, context,
      {.name = "prefab_set_socket",
       .title = "Edit a socket",
       .description =
           "Changes a socket (by name or index): name, parent node "
           "(keeps its model-space placement unless position / rotation "
           "are given too), local position, local rotation_deg.",
       .input_schema = SocketSchema(true)},
      [ctx](const ToolCall& call) -> ToolReturn {
        PrefabResult prefab = PrefabForEdit(*ctx);
        if (!prefab.ok()) {
          return prefab.error();
        }
        PrefabDocument& doc = *prefab.value();
        const prefab::Prefab& state = doc.state();
        const Json& args = call.arguments;
        IndexResult socket = ResolveIndex(
            args.Find("socket"), state.sockets.size(),
            [&](size_t i) -> const std::string& {
              return state.sockets[i].name;
            },
            "socket");
        if (!socket.ok()) {
          return ToolResult::Error(socket.error());
        }
        const int index = socket.value();
        const Json* node_arg = args.Find("node");
        IndexResult node = ResolveNode(state, node_arg);
        if (!node.ok()) {
          return ToolResult::Error(node.error());
        }
        const std::string* name = args.GetString("name");
        if (name != nullptr) {
          for (size_t i = 0; i < state.sockets.size(); ++i) {
            if (static_cast<int>(i) != index &&
                state.sockets[i].name == *name) {
              return ToolResult::Error("another socket has that name");
            }
          }
          if (name->empty()) {
            return ToolResult::Error("name is empty");
          }
        }
        // Check position / rotation before the edit starts.
        animation::Transform probe;
        if (ArgsStatus ok = ApplyLocalArgs(args, probe); !ok.ok()) {
          return ToolResult::Error(ok.error());
        }
        bool moved = true;
        doc.Edit(
            "Edit socket", ChangeKind::kParameters, [&](prefab::Prefab& p) {
              if (node_arg != nullptr) {
                moved =
                    MoveToNode(p, AttachmentKind::kSocket, index, node.value());
              }
              prefab::Socket& s = p.sockets[static_cast<size_t>(index)];
              (void)ApplyLocalArgs(args, s.local);
              if (name != nullptr) {
                s.name = *name;
              }
            });
        doc.selection().Set({.kind = SelectionKind::kSocket, .index = index});
        Json result = SocketJson(doc.state(), index);
        result.Set("undo", UndoNote(doc));
        if (!moved) {
          result.Set("warning", "the socket could not be moved to that node");
        }
        return ToolResult::Structured(std::move(result));
      });

  AddTool(
      server, context,
      {.name = "prefab_set_collider",
       .title = "Set the collider",
       .description =
           "Sets the prefab's physics: collider kind (none, box, sphere, "
           "capsule, hull = convex hull, mesh = triangle mesh, static only), "
           "fitted to the prefab bounds and cooked; and / or the body: "
           "dynamic, mass (kg, > 0), friction (>= 0), restitution (0..1). "
           "Returns the physics settings.",
       .input_schema = ObjectSchema(
           Json::Object()
               .Set("kind", EnumProp({"none", "box", "sphere", "capsule",
                                      "hull", "mesh"},
                                     "Collider shape."))
               .Set("dynamic", Prop("boolean", "Moved by physics."))
               .Set("mass", Prop("number", "kg, > 0."))
               .Set("friction", Prop("number", ">= 0"))
               .Set("restitution", Prop("number", "0..1")))},
      [ctx](const ToolCall& call) -> ToolReturn {
        PrefabResult prefab = PrefabForEdit(*ctx);
        if (!prefab.ok()) {
          return prefab.error();
        }
        PrefabDocument& doc = *prefab.value();
        const Json& args = call.arguments;
        // Edit a copy (cheap: bulk data is shared) so a failed cook leaves
        // the document alone.
        prefab::Prefab edited = doc.state();
        core::Result<int, std::string> body =
            ApplyBodyArgs(args, edited.physics);
        if (!body.ok()) {
          return ToolResult::Error(body.error());
        }
        const std::string* kind_name = args.GetString("kind");
        if (kind_name == nullptr && body.value() == 0) {
          return ToolResult::Error("give kind and / or body parameters");
        }
        if (kind_name != nullptr) {
          const std::optional<prefab::ColliderKind> kind =
              ParseColliderKindArg(*kind_name);
          if (!kind) {
            return ToolResult::Error("unknown collider kind");
          }
          prefab::Physics settings = edited.physics;
          settings.kind = *kind;
          if (*kind == prefab::ColliderKind::kTriangleMesh) {
            settings.dynamic = false;
          }
          core::Result<void, cook::CookError> cooked =
              cook::CookCollision(edited, settings, cook::ShapeFit::kBounds);
          if (!cooked.ok()) {
            return ToolResult::Error("Cannot cook the collider: " +
                                     CookErrorText(cooked.error()));
          }
        }
        doc.Edit(kind_name != nullptr ? "Collider kind" : "Edit physics",
                 ChangeKind::kParameters,
                 [&](prefab::Prefab& p) { p = std::move(edited); });
        doc.selection().Set({.kind = SelectionKind::kCollider, .index = -1});
        Json result =
            *PrefabToJson(doc.state(), ctx->asset_root()).GetObject("physics");
        result.Set("undo", UndoNote(doc));
        return ToolResult::Structured(std::move(result));
      });

  AddTool(
      server, context,
      {.name = "prefab_clip_edit",
       .title = "Edit animation clips",
       .description =
           "Edits the prefab's animation clips (clips by name or index): "
           "action merge (clips: >= 2 clips, name: the new clip), trim "
           "(clip, start, end in seconds), loop (clip, loop), add_event "
           "(clip, event name, time), remove_event (clip, event_index). "
           "Returns the clips.",
       .input_schema = ObjectSchema(
           Json::Object()
               .Set("action", EnumProp({"merge", "trim", "loop", "add_event",
                                        "remove_event"},
                                       "What to do."))
               .Set("clip", AnyProp("Clip name or index."))
               .Set("clips", Prop("array", "merge: clip names or indices."))
               .Set("name", Prop("string", "merge: name of the new clip."))
               .Set("start", Prop("number", "trim: start in seconds."))
               .Set("end", Prop("number", "trim: end in seconds."))
               .Set("loop", Prop("boolean", "loop: the loop flag."))
               .Set("event", Prop("string", "add_event: event name."))
               .Set("time", Prop("number", "add_event: time in seconds."))
               .Set("event_index",
                    Prop("integer", "remove_event: index of the event.")),
           {"action"})},
      [ctx](const ToolCall& call) -> ToolReturn {
        PrefabResult prefab = PrefabForEdit(*ctx);
        if (!prefab.ok()) {
          return prefab.error();
        }
        PrefabDocument& doc = *prefab.value();
        core::Result<std::string, std::string> line =
            ClipEditCommand(call.arguments, doc.state());
        if (!line.ok()) {
          return ToolResult::Error(line.error());
        }
        const CommandOutcome outcome = RunCommandLine(*ctx, line.value());
        if (!outcome.ok) {
          return OutcomeResult(outcome, "");
        }
        const Json json = PrefabToJson(doc.state(), ctx->asset_root());
        return ToolResult::Structured(Json::Object()
                                          .Set("clips", *json.GetArray("clips"))
                                          .Set("undo", UndoNote(doc)));
      });

  AddTool(server, context,
          {.name = "prefab_validate",
           .title = "Validate the prefab",
           .description =
               "Checks whether the open prefab can be saved (indices, "
               "duplicate socket or clip names, dynamic bodies without mass, "
               "mesh colliders without a cooked shape). Returns ok and the "
               "problem.",
           .input_schema = ObjectSchema(Json::Object()),
           .read_only = true},
          [ctx](const ToolCall&) -> ToolReturn {
            PrefabResult prefab = RequirePrefab(*ctx);
            if (!prefab.ok()) {
              return prefab.error();
            }
            const std::optional<std::string> problem =
                CheckBeforeSave(prefab.value()->state());
            return ToolResult::Structured(
                Json::Object()
                    .Set("ok", !problem.has_value())
                    .Set("problem",
                         problem ? Json::String(*problem) : Json::Null()));
          });
}

}  // namespace blue::editor::mcp_tools
