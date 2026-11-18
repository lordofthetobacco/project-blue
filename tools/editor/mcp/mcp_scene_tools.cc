// The editor's MCP tools for the scene document (Scene mode): thin wrappers
// over the pure edits of scene_edit.h / scene_data.h, each one undo step.
//
//   scene_get, scene_add_instance, scene_set_transform, scene_delete,
//   scene_duplicate, scene_set_material_override, scene_set_sun,
//   scene_set_environment, scene_add_point_light, scene_validate

#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glm/vec3.hpp>

#include "core/result.h"
#include "editor/dialogs.h"
#include "editor/document.h"
#include "editor/editor_context.h"
#include "editor/mcp/mcp_args.h"
#include "editor/mcp/mcp_export.h"
#include "editor/mcp/mcp_tool_util.h"
#include "editor/panel.h"
#include "editor/scene_mode/scene_edit.h"
#include "editor/selection.h"
#include "editor/undo.h"
#include "editor/viewport/editor_camera.h"
#include "editor/viewport/viewport_panel.h"
#include "mcp/json.h"
#include "mcp/server.h"
#include "platform/file_system.h"
#include "prefab/prefab.h"
#include "prefab/prefab_file.h"
#include "scene/scene.h"
#include "scene/scene_data.h"

namespace blue::editor::mcp_tools {
namespace {

// How deep scene_validate follows child prefabs for material names.
constexpr int kMaxChildDepth = 8;

using SceneResult = core::Result<SceneDocument*, ToolResult>;

// The scene document, switching to Scene mode so the user sees the edit.
SceneResult SceneForEdit(EditorContext& context) {
  SceneResult scene = RequireScene(context);
  if (scene.ok() && context.mode() != EditorMode::kScene) {
    context.SetMode(EditorMode::kScene);
  }
  return scene;
}

// Returns an error result unless instance `id` exists in `scene`.
std::optional<ToolResult> CheckInstance(const scene::SceneData& scene,
                                        scene::InstanceId id) {
  if (scene::FindInstance(scene, id) == nullptr) {
    return ToolResult::Error(
        std::format("No instance with id {} (see scene_get).", id));
  }
  return std::nullopt;
}

// Reads the required integer "id" (the schema guarantees an integer).
scene::InstanceId InstanceIdArg(const Json& args) {
  const int64_t id = args.GetInteger("id").value_or(0);
  return id > 0 && id <= int64_t{UINT32_MAX}
             ? static_cast<scene::InstanceId>(id)
             : scene::kNoInstance;
}

// Edit result: what was done, the undo step, and the instance as it is now.
ToolResult InstanceResult(const SceneDocument& doc, scene::InstanceId id,
                          std::string_view done) {
  Json result = Json::Object().Set("done", done).Set("undo", UndoNote(doc));
  if (const scene::Instance* instance = scene::FindInstance(doc.state(), id)) {
    result.Set("instance",
               Json::Object()
                   .Set("id", static_cast<int64_t>(instance->id))
                   .Set("name", instance->name)
                   .Set("transform", TransformJson(instance->transform)));
  }
  return ToolResult::Structured(std::move(result));
}

// Returns member `key` of the scene's JSON (SceneToJson).
Json SceneMember(const SceneDocument& doc, const EditorContext& context,
                 std::string_view key) {
  const Json scene = SceneToJson(doc.state(), context.asset_root());
  const Json* member = scene.GetObject(key);
  return member != nullptr ? *member : Json::Object();
}

// Collects the material names of the prefab at `path` and its child
// prefabs into `names`; returns false if `path` cannot be loaded.
bool CollectMaterialNames(const std::string& path, int depth,
                          std::vector<std::string>& names, std::string& error) {
  core::Result<prefab::Prefab, prefab::PrefabError> loaded =
      prefab::LoadPrefab(path);
  if (!loaded.ok()) {
    error = prefab::ToString(loaded.error());
    return false;
  }
  for (const prefab::Material& material : loaded.value().materials) {
    names.push_back(material.name);
  }
  if (depth < kMaxChildDepth) {
    for (const prefab::ChildPrefab& child : loaded.value().children) {
      std::string ignored;
      (void)CollectMaterialNames(child.path, depth + 1, names, ignored);
    }
  }
  return true;
}

Json ValidateScene(const scene::SceneData& scene) {
  Json problems = Json::Array();
  const auto add = [&problems](scene::InstanceId instance,
                               std::string message) {
    Json problem = Json::Object().Set("message", std::move(message));
    if (instance != scene::kNoInstance) {
      problem.Set("instance", static_cast<int64_t>(instance));
    }
    problems.Push(std::move(problem));
  };
  if (std::optional<std::string> error = scene::ValidateSceneData(scene)) {
    add(scene::kNoInstance, "Cannot be saved: " + *error);
  }
  std::vector<std::vector<std::string>> material_names(scene.prefabs.size());
  std::vector<bool> loadable(scene.prefabs.size(), true);
  for (size_t i = 0; i < scene.prefabs.size(); ++i) {
    std::string error;
    loadable[i] =
        CollectMaterialNames(scene.prefabs[i], 0, material_names[i], error);
    if (!loadable[i]) {
      add(scene::kNoInstance, std::format("Prefab {} cannot be loaded: {}",
                                          scene.prefabs[i], error));
    }
  }
  // Unloadable prefabs are reported above; do not check their materials.
  scene::SceneData checked = scene;
  for (scene::Instance& instance : checked.instances) {
    if (instance.prefab != scene::kNoPrefab &&
        !loadable[static_cast<size_t>(instance.prefab)]) {
      instance.materials.clear();
    }
  }
  for (const scene::SceneIssue& issue :
       scene::ValidateAgainstPrefabs(checked, material_names)) {
    add(issue.instance, issue.message);
  }
  const bool ok = problems.size() == 0;
  return Json::Object().Set("ok", ok).Set("problems", std::move(problems));
}

}  // namespace

void RegisterSceneTools(mcp::Server& server, EditorContext& context) {
  EditorContext* ctx = &context;

  AddTool(server, context,
          {.name = "scene_get",
           .title = "Get the scene",
           .description =
               "Returns the open scene as JSON: prefabs; instances (id, name, "
               "prefab path or null for a group, parent id, local transform "
               "with position, rotation quaternion [x,y,z,w], rotation_deg "
               "Euler XYZ and scale, world_position, visible, "
               "material_overrides, physics_override); point_lights (index, "
               "name, position, color, intensity, range); sun (direction, "
               "elevation_deg, azimuth_deg, color, intensity, "
               "casts_shadows); environment (hdri, intensity, rotation_deg, "
               "exposure_ev); player_start; gravity. Units: metres, degrees.",
           .input_schema = ObjectSchema(Json::Object()),
           .read_only = true},
          [ctx](const ToolCall&) -> ToolReturn {
            SceneResult scene = RequireScene(*ctx);
            if (!scene.ok()) {
              return scene.error();
            }
            return ToolResult::Structured(
                SceneToJson(scene.value()->state(), ctx->asset_root()));
          });

  AddTool(
      server, context,
      {
          .name = "scene_add_instance",
          .title = "Add a prefab instance",
          .description =
              "Places an instance of a prefab (.pbo) in the scene and selects "
              "it. Without a position it lands where the viewport camera looks "
              "(on the ground). Returns the new instance with its id.",
          .input_schema = ObjectSchema(
              Json::Object()
                  .Set("prefab", Prop("string",
                                      "The .pbo file; relative to the asset "
                                      "root."))
                  .Set("position",
                       VectorProp(3,
                                  "World position [x, y, z] (in the parent's "
                                  "space when parent is given)."))
                  .Set("parent",
                       Prop("integer", "Id of the instance to place it under."))
                  .Set("name",
                       Prop("string",
                            "Display name (default: the file name, made "
                            "unique)."))),
      },
      [ctx](const ToolCall& call) -> ToolReturn {
        SceneResult scene = SceneForEdit(*ctx);
        if (!scene.ok()) {
          return scene.error();
        }
        const Json& args = call.arguments;
        const std::string path = platform::AbsolutePath(
            ResolveAssetPath(*ctx, *args.GetString("prefab")));
        if (!MatchesFileKind(path, FileKind::kPrefab) ||
            !platform::IsRegularFile(path)) {
          return ToolResult::Error(
              std::format("{} is not an existing .pbo file", path));
        }
        auto position = GetVec3Arg(args, "position");
        if (!position.ok()) {
          return ToolResult::Error(position.error());
        }
        scene::InstanceId parent = scene::kNoInstance;
        if (const std::optional<int64_t> id = args.GetInteger("parent")) {
          parent = static_cast<scene::InstanceId>(*id);
          if (std::optional<ToolResult> bad =
                  CheckInstance(scene.value()->state(), parent)) {
            return bad;
          }
        }
        glm::vec3 where;
        if (position.value()) {
          where = *position.value();
        } else {
          const EditorCamera& camera = ctx->viewport().camera();
          where = PlacementPoint(camera.position(), camera.forward());
        }
        const std::string* name = args.GetString("name");
        scene::InstanceId id = scene::kNoInstance;
        SceneDocument& doc = *scene.value();
        doc.Edit("Add instance", ChangeKind::kStructure,
                 [&](scene::SceneData& data) {
                   id = PlacePrefab(data, path, where, parent);
                   scene::Instance* added = scene::FindInstance(data, id);
                   if (added != nullptr && name != nullptr && !name->empty()) {
                     added->name = *name;
                   }
                 });
        if (id == scene::kNoInstance) {  // Not expected: checked above.
          return ToolResult::Error("The instance could not be placed.");
        }
        doc.selection().Set(InstanceItem(id));
        return InstanceResult(doc, id, "Added instance");
      });

  AddTool(
      server, context,
      {.name = "scene_set_transform",
       .title = "Transform an instance",
       .description =
           "Sets the local transform (relative to its parent) of a scene "
           "instance: any of position, rotation_deg (Euler XYZ degrees) "
           "or rotation (quaternion [x,y,z,w]), scale (a number or "
           "[x,y,z]). Unspecified parts stay.",
       .input_schema = ObjectSchema(
           Json::Object()
               .Set("id", Prop("integer", "Instance id (scene_get)."))
               .Set("position", VectorProp(3, "[x, y, z] in metres."))
               .Set("rotation_deg",
                    VectorProp(3, "Euler angles [x, y, z] in degrees."))
               .Set("rotation", VectorProp(4, "Quaternion [x, y, z, w]."))
               .Set("scale", AnyProp("Uniform scale (number) or [x, y, "
                                     "z]; each > 0.")),
           {"id"})},
      [ctx](const ToolCall& call) -> ToolReturn {
        SceneResult scene = SceneForEdit(*ctx);
        if (!scene.ok()) {
          return scene.error();
        }
        SceneDocument& doc = *scene.value();
        const scene::InstanceId id = InstanceIdArg(call.arguments);
        if (std::optional<ToolResult> bad = CheckInstance(doc.state(), id)) {
          return bad;
        }
        scene::Transform transform =
            scene::FindInstance(doc.state(), id)->transform;
        if (ArgsStatus applied = ApplyTransformArgs(call.arguments, transform);
            !applied.ok()) {
          return ToolResult::Error(applied.error());
        }
        doc.Edit("Transform instance", ChangeKind::kParameters,
                 [&](scene::SceneData& data) {
                   scene::FindInstance(data, id)->transform = transform;
                 });
        return InstanceResult(doc, id, "Transformed instance");
      });

  const Json ids_schema = ObjectSchema(
      Json::Object().Set(
          "ids", AnyProp("An instance id or an array of ids (scene_get).")),
      {"ids"});
  AddTool(
      server, context,
      {.name = "scene_delete",
       .title = "Delete instances",
       .description =
           "Deletes scene instances together with their child instances "
           "(one undo step).",
       .input_schema = ids_schema,
       .destructive = true},
      [ctx](const ToolCall& call) -> ToolReturn {
        SceneResult scene = SceneForEdit(*ctx);
        if (!scene.ok()) {
          return scene.error();
        }
        SceneDocument& doc = *scene.value();
        auto ids = GetInstanceIdsArg(call.arguments, "ids");
        if (!ids.ok() || ids.value().empty()) {
          return ToolResult::Error("ids must name at least one instance");
        }
        for (const scene::InstanceId id : ids.value()) {
          if (std::optional<ToolResult> bad = CheckInstance(doc.state(), id)) {
            return bad;
          }
        }
        int removed = 0;
        doc.Edit("Delete", ChangeKind::kStructure, [&](scene::SceneData& data) {
          removed = DeleteInstances(data, ids.value());
        });
        doc.selection().PruneInstances(doc.state());
        return ToolResult::Text(
            std::format("Deleted {} instance(s) with their children. {}",
                        removed, UndoNote(doc)));
      });

  AddTool(
      server, context,
      {.name = "scene_duplicate",
       .title = "Duplicate instances",
       .description =
           "Duplicates scene instances (with their children), moved by "
           "offset (default [1, 0, 0]), and selects the copies. Returns "
           "the ids of the copies.",
       .input_schema = ObjectSchema(
           Json::Object()
               .Set("ids", AnyProp("An instance id or an array of ids."))
               .Set("offset",
                    VectorProp(3, "World offset of the copies [x, y, z].")),
           {"ids"})},
      [ctx](const ToolCall& call) -> ToolReturn {
        SceneResult scene = SceneForEdit(*ctx);
        if (!scene.ok()) {
          return scene.error();
        }
        SceneDocument& doc = *scene.value();
        auto ids = GetInstanceIdsArg(call.arguments, "ids");
        auto offset = GetVec3Arg(call.arguments, "offset");
        if (!ids.ok() || ids.value().empty() || !offset.ok()) {
          return ToolResult::Error(!offset.ok()
                                       ? offset.error()
                                       : "ids must name at least one instance");
        }
        for (const scene::InstanceId id : ids.value()) {
          if (std::optional<ToolResult> bad = CheckInstance(doc.state(), id)) {
            return bad;
          }
        }
        std::vector<scene::InstanceId> copies;
        doc.Edit("Duplicate", ChangeKind::kStructure,
                 [&](scene::SceneData& data) {
                   copies = DuplicateInstances(
                       data, ids.value(),
                       offset.value().value_or(glm::vec3(1.0f, 0.0f, 0.0f)));
                 });
        Json list = Json::Array();
        doc.selection().Clear();
        for (const scene::InstanceId id : copies) {
          list.Push(Json::Integer(id));
          doc.selection().Add(InstanceItem(id));
        }
        return ToolResult::Structured(Json::Object()
                                          .Set("copies", std::move(list))
                                          .Set("undo", UndoNote(doc)));
      });

  AddTool(
      server, context,
      {.name = "scene_set_material_override",
       .title = "Override an instance material",
       .description =
           "Changes how one material (by name, see prefab_get of the "
           "instance's prefab) looks on one instance, without touching the "
           "prefab. Optionally replaces it by a material of another prefab "
           "(replace_prefab + replace_material; replace_prefab \"\" makes it "
           "parameter-only again), then applies parameter tweaks: "
           "base_color [r,g,b,a] 0..1, metallic, roughness 0..1, emissive "
           "[r,g,b], emissive_strength, uv_scale [u,v]. clear: true removes "
           "the override.",
       .input_schema = ObjectSchema(
           Json::Object()
               .Set("id", Prop("integer", "Instance id."))
               .Set("material", Prop("string", "Material name."))
               .Set("clear", Prop("boolean", "Remove the override."))
               .Set("replace_prefab", Prop("string",
                                           "Material library .pbo (asset-root "
                                           "relative)."))
               .Set("replace_material",
                    Prop("string", "Material name in replace_prefab."))
               .Set("base_color", VectorProp(4, "Linear RGBA, 0..1."))
               .Set("metallic", Prop("number", "0..1"))
               .Set("roughness", Prop("number", "0..1"))
               .Set("emissive", VectorProp(3, "Linear RGB, >= 0."))
               .Set("emissive_strength", Prop("number", ">= 0"))
               .Set("uv_scale", VectorProp(2, "[u, v], > 0.")),
           {"id", "material"})},
      [ctx](const ToolCall& call) -> ToolReturn {
        SceneResult scene = SceneForEdit(*ctx);
        if (!scene.ok()) {
          return scene.error();
        }
        SceneDocument& doc = *scene.value();
        const Json& args = call.arguments;
        const scene::InstanceId id = InstanceIdArg(args);
        if (std::optional<ToolResult> bad = CheckInstance(doc.state(), id)) {
          return bad;
        }
        const std::string material = *args.GetString("material");
        if (material.empty()) {
          return ToolResult::Error("material must not be empty");
        }
        if (args.GetBool("clear").value_or(false)) {
          if (scene::FindMaterialOverride(doc.state(), id, material) ==
              nullptr) {
            return ToolResult::Error("The material has no override.");
          }
          doc.Edit("Clear override", ChangeKind::kStructure,
                   [&](scene::SceneData& data) {
                     (void)RemoveMaterialOverride(data, id, material);
                   });
          return ToolResult::Text("Override removed. " + UndoNote(doc));
        }
        // Validate everything on a copy of the override first.
        scene::MaterialOverride tweaked;
        if (const scene::MaterialOverride* existing =
                scene::FindMaterialOverride(doc.state(), id, material)) {
          tweaked = *existing;
        }
        core::Result<int, std::string> tweaks =
            ApplyOverrideArgs(args, tweaked);
        if (!tweaks.ok()) {
          return ToolResult::Error(tweaks.error());
        }
        const std::string* replace_prefab = args.GetString("replace_prefab");
        const std::string* replace_material =
            args.GetString("replace_material");
        std::string library;
        if (replace_prefab != nullptr && !replace_prefab->empty()) {
          library =
              platform::AbsolutePath(ResolveAssetPath(*ctx, *replace_prefab));
          if (!platform::IsRegularFile(library) ||
              replace_material == nullptr || replace_material->empty()) {
            return ToolResult::Error(
                "replace_prefab must be an existing .pbo and "
                "replace_material a material name in it");
          }
        }
        if (tweaks.value() == 0 && replace_prefab == nullptr) {
          return ToolResult::Error(
              "Give parameters, replace_prefab + replace_material, or clear.");
        }
        doc.Edit("Material override", ChangeKind::kStructure,
                 [&](scene::SceneData& data) {
                   scene::Instance* instance = scene::FindInstance(data, id);
                   scene::MaterialOverride& target =
                       FindOrAddMaterialOverride(*instance, material);
                   const int source = target.source_prefab;
                   const std::string source_material = target.source_material;
                   target = tweaked;
                   target.material_name = material;
                   target.source_prefab = source;
                   target.source_material = source_material;
                   if (replace_prefab != nullptr) {
                     (void)SetMaterialReplacement(
                         data, id, material, library,
                         library.empty() ? std::string() : *replace_material);
                   }
                 });
        Json result = Json::Object().Set("undo", UndoNote(doc));
        if (const scene::Instance* instance =
                scene::FindInstance(doc.state(), id)) {
          result.Set("instance", Json::Object()
                                     .Set("id", static_cast<int64_t>(id))
                                     .Set("name", instance->name));
        }
        const Json scene_json = SceneToJson(doc.state(), ctx->asset_root());
        for (const Json& item :
             scene_json.GetArray("instances")->array_items()) {
          if (item.GetInteger("id") == int64_t{id}) {
            if (const Json* overrides = item.GetArray("material_overrides")) {
              result.Set("material_overrides", *overrides);
            }
          }
        }
        return ToolResult::Structured(std::move(result));
      });

  AddTool(
      server, context,
      {.name = "scene_set_sun",
       .title = "Set the sun",
       .description =
           "Changes the scene's sun: elevation_deg (-90..90, above the "
           "horizon) and / or azimuth_deg (from +Z towards +X), or a "
           "light travel direction [x,y,z]; color [r,g,b] linear; "
           "intensity; casts_shadows.",
       .input_schema = ObjectSchema(
           Json::Object()
               .Set("elevation_deg", Prop("number", "-90..90"))
               .Set("azimuth_deg", Prop("number", "Degrees."))
               .Set("direction",
                    VectorProp(3,
                               "Light travel direction (not with the "
                               "angles)."))
               .Set("color", VectorProp(3, "Linear RGB, >= 0."))
               .Set("intensity", Prop("number", ">= 0"))
               .Set("casts_shadows", Prop("boolean", "Shadows on / off.")))},
      [ctx](const ToolCall& call) -> ToolReturn {
        SceneResult scene = SceneForEdit(*ctx);
        if (!scene.ok()) {
          return scene.error();
        }
        SceneDocument& doc = *scene.value();
        scene::SunLight sun = doc.state().sun;
        if (ArgsStatus applied = ApplySunArgs(call.arguments, sun);
            !applied.ok()) {
          return ToolResult::Error(applied.error());
        }
        doc.Edit("Sun", ChangeKind::kParameters,
                 [&](scene::SceneData& data) { data.sun = sun; });
        Json sun_json = SceneMember(doc, *ctx, "sun");
        sun_json.Set("undo", UndoNote(doc));
        return ToolResult::Structured(std::move(sun_json));
      });

  AddTool(server, context,
          {.name = "scene_set_environment",
           .title = "Set the environment",
           .description =
               "Changes the sky / image-based lighting: hdri (an .hdr file, "
               "asset-root relative; \"\" for the built-in default sky), "
               "intensity, rotation_deg about +Y, exposure_ev (camera "
               "exposure compensation, -20..20).",
           .input_schema = ObjectSchema(
               Json::Object()
                   .Set("hdri", Prop("string", ".hdr file or \"\"."))
                   .Set("intensity", Prop("number", ">= 0"))
                   .Set("rotation_deg", Prop("number", "Degrees about +Y."))
                   .Set("exposure_ev", Prop("number", "-20..20")))},
          [ctx](const ToolCall& call) -> ToolReturn {
            SceneResult scene = SceneForEdit(*ctx);
            if (!scene.ok()) {
              return scene.error();
            }
            SceneDocument& doc = *scene.value();
            scene::Environment environment = doc.state().environment;
            if (ArgsStatus applied =
                    ApplyEnvironmentArgs(call.arguments, environment);
                !applied.ok()) {
              return ToolResult::Error(applied.error());
            }
            if (const std::string* hdri = call.arguments.GetString("hdri")) {
              if (hdri->empty()) {
                environment.hdri_path.clear();
              } else {
                const std::string path =
                    platform::AbsolutePath(ResolveAssetPath(*ctx, *hdri));
                if (!MatchesFileKind(path, FileKind::kHdr) ||
                    !platform::IsRegularFile(path)) {
                  return ToolResult::Error(
                      std::format("{} is not an existing .hdr file", path));
                }
                environment.hdri_path = path;
              }
            }
            doc.Edit("Environment", ChangeKind::kParameters,
                     [&](scene::SceneData& data) {
                       data.environment = environment;
                     });
            Json environment_json = SceneMember(doc, *ctx, "environment");
            environment_json.Set("undo", UndoNote(doc));
            return ToolResult::Structured(std::move(environment_json));
          });

  AddTool(
      server, context,
      {.name = "scene_add_point_light",
       .title = "Add a point light",
       .description =
           "Adds a point light to the scene and selects it. Returns its "
           "index in point_lights.",
       .input_schema = ObjectSchema(
           Json::Object()
               .Set("position", VectorProp(3, "[x, y, z] in metres."))
               .Set("color", VectorProp(3, "Linear RGB (default white)."))
               .Set("intensity", Prop("number", ">= 0 (default 10)."))
               .Set("range", Prop("number", "Metres, > 0 (default 10)."))
               .Set("name", Prop("string", "Display name.")),
           {"position"})},
      [ctx](const ToolCall& call) -> ToolReturn {
        SceneResult scene = SceneForEdit(*ctx);
        if (!scene.ok()) {
          return scene.error();
        }
        SceneDocument& doc = *scene.value();
        scene::ScenePointLight light;
        light.name =
            std::format("light {}", doc.state().point_lights.size() + 1);
        if (ArgsStatus applied = ApplyPointLightArgs(call.arguments, light);
            !applied.ok()) {
          return ToolResult::Error(applied.error());
        }
        const size_t index = doc.state().point_lights.size();
        doc.Edit("Add light", ChangeKind::kStructure,
                 [&](scene::SceneData& data) {
                   data.point_lights.push_back(light);
                 });
        doc.selection().Set({.kind = SelectionKind::kScenePointLight,
                             .index = static_cast<int64_t>(index)});
        return ToolResult::Text(std::format("Added point light {} ('{}'). {}",
                                            index, light.name, UndoNote(doc)));
      });

  AddTool(
      server, context,
      {.name = "scene_validate",
       .title = "Validate the scene",
       .description =
           "Checks the open scene: whether it can be saved, prefabs "
           "that cannot be loaded, material overrides naming materials "
           "the prefab does not have. Returns ok and a problems list "
           "(message, instance id).",
       .input_schema = ObjectSchema(Json::Object()),
       .read_only = true},
      [ctx](const ToolCall&) -> ToolReturn {
        SceneResult scene = RequireScene(*ctx);
        if (!scene.ok()) {
          return scene.error();
        }
        return ToolResult::Structured(ValidateScene(scene.value()->state()));
      });
}

}  // namespace blue::editor::mcp_tools
