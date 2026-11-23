#include "editor/mcp/mcp_export.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <glm/gtc/quaternion.hpp>
#include <glm/trigonometric.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "animation/clip.h"
#include "animation/transform.h"
#include "editor/document.h"
#include "editor/prefab_mode/prefab_edit.h"
#include "material/material.h"
#include "mcp/json.h"
#include "prefab/prefab.h"
#include "scene/scene.h"
#include "scene/scene_data.h"

namespace blue::editor {
namespace {

using mcp::Json;

constexpr std::array<const char*, prefab::kTextureSlotCount> kSlotNames{
    "base_color", "orm", "normal", "emissive"};
constexpr std::array<const char*, 6> kColliderNames{"none",    "box",  "sphere",
                                                    "capsule", "hull", "mesh"};

Json QuatJson(const glm::quat& q) {
  return Json::Array()
      .Push(Json::Number(q.x))
      .Push(Json::Number(q.y))
      .Push(Json::Number(q.z))
      .Push(Json::Number(q.w));
}

Json EulerDegreesJson(const glm::quat& q) {
  return Vec3Json(glm::degrees(glm::eulerAngles(q)));
}

Json BoundsJson(const asset::Bounds& bounds) {
  return Json::Object()
      .Set("min", Vec3Json(bounds.min))
      .Set("max", Vec3Json(bounds.max));
}

Json OverrideJson(const scene::SceneData& scene,
                  const scene::MaterialOverride& material,
                  const std::string& asset_root) {
  Json json = Json::Object().Set("material", material.material_name);
  if (material.source_prefab != scene::kNoPrefab &&
      static_cast<size_t>(material.source_prefab) < scene.prefabs.size()) {
    json.Set(
        "replace_with",
        Json::Object()
            .Set("prefab",
                 DisplayPath(
                     scene.prefabs[static_cast<size_t>(material.source_prefab)],
                     asset_root))
            .Set("material", material.source_material));
  }
  if (material.base_color) {
    json.Set("base_color", Vec4Json(*material.base_color));
  }
  if (material.metallic) {
    json.Set("metallic", static_cast<double>(*material.metallic));
  }
  if (material.roughness) {
    json.Set("roughness", static_cast<double>(*material.roughness));
  }
  if (material.emissive) {
    json.Set("emissive", Vec3Json(*material.emissive));
  }
  if (material.emissive_strength) {
    json.Set("emissive_strength",
             static_cast<double>(*material.emissive_strength));
  }
  if (material.uv_scale) {
    json.Set("uv_scale", Vec2Json(*material.uv_scale));
  }
  return json;
}

Json PhysicsOverrideJson(const scene::PhysicsOverride& physics) {
  Json json = Json::Object().Set("disabled", physics.disabled);
  if (physics.motion) {
    json.Set("motion", *physics.motion == scene::MotionType::kDynamic
                           ? "dynamic"
                           : "static");
  }
  if (physics.mass) {
    json.Set("mass", static_cast<double>(*physics.mass));
  }
  if (physics.friction) {
    json.Set("friction", static_cast<double>(*physics.friction));
  }
  if (physics.restitution) {
    json.Set("restitution", static_cast<double>(*physics.restitution));
  }
  return json;
}

Json InstanceJson(const scene::SceneData& scene,
                  const scene::Instance& instance,
                  const std::string& asset_root) {
  Json json = Json::Object()
                  .Set("id", static_cast<int64_t>(instance.id))
                  .Set("name", instance.name);
  if (instance.prefab != scene::kNoPrefab &&
      static_cast<size_t>(instance.prefab) < scene.prefabs.size()) {
    json.Set("prefab",
             DisplayPath(scene.prefabs[static_cast<size_t>(instance.prefab)],
                         asset_root));
  } else {
    json.Set("prefab", Json::Null());  // A group node.
  }
  json.Set("parent", instance.parent == scene::kNoInstance
                         ? Json::Null()
                         : Json::Integer(instance.parent));
  json.Set("transform", TransformJson(instance.transform));
  if (const std::optional<glm::mat4> world =
          scene::WorldMatrix(scene, instance.id)) {
    json.Set("world_position", Vec3Json(glm::vec3((*world)[3])));
  }
  json.Set("visible", instance.visible);
  if (!instance.materials.empty()) {
    Json overrides = Json::Array();
    for (const scene::MaterialOverride& material : instance.materials) {
      overrides.Push(OverrideJson(scene, material, asset_root));
    }
    json.Set("material_overrides", std::move(overrides));
  }
  if (instance.physics) {
    json.Set("physics_override", PhysicsOverrideJson(*instance.physics));
  }
  return json;
}

Json SunJson(const scene::SunLight& sun) {
  const scene::SunAngles angles = scene::SunAnglesFromDirection(sun.direction);
  return Json::Object()
      .Set("direction", Vec3Json(sun.direction))
      .Set("elevation_deg", static_cast<double>(glm::degrees(angles.elevation)))
      .Set("azimuth_deg", static_cast<double>(glm::degrees(angles.azimuth)))
      .Set("color", Vec3Json(sun.color))
      .Set("intensity", static_cast<double>(sun.intensity))
      .Set("casts_shadows", sun.casts_shadows);
}

Json MaterialJson(const prefab::Prefab& prefab, const prefab::Material& m,
                  int index) {
  Json textures = Json::Object();
  for (int slot = 0; slot < prefab::kTextureSlotCount; ++slot) {
    const auto s = static_cast<size_t>(slot);
    const int texture = m.textures[s];
    if (texture < 0 || static_cast<size_t>(texture) >= prefab.textures.size()) {
      continue;
    }
    textures.Set(
        kSlotNames[s],
        Json::Object()
            .Set("texture", texture)
            .Set("name", prefab.textures[static_cast<size_t>(texture)].name)
            .Set("uv_set", m.uv_sets[s]));
  }
  return Json::Object()
      .Set("index", index)
      .Set("name", m.name)
      .Set("base_color", Vec4Json(m.base_color))
      .Set("metallic", static_cast<double>(m.metallic))
      .Set("roughness", static_cast<double>(m.roughness))
      .Set("emissive", Vec3Json(m.emissive))
      .Set("emissive_strength", static_cast<double>(m.emissive_strength))
      .Set("normal_scale", static_cast<double>(m.normal_scale))
      .Set("occlusion_strength", static_cast<double>(m.occlusion_strength))
      .Set("uv_scale", Vec2Json(m.uv_scale))
      .Set("alpha_mode",
           m.alpha_mode == material::AlphaMode::kMask ? "mask" : "opaque")
      .Set("alpha_cutoff", static_cast<double>(m.alpha_cutoff))
      .Set("double_sided", m.double_sided)
      .Set("textures", std::move(textures));
}

Json MeshJson(const prefab::Mesh& mesh, int index) {
  Json submeshes = Json::Array();
  for (const asset::Submesh& submesh : mesh.submeshes) {
    submeshes.Push(
        Json::Object()
            .Set("material", submesh.material)
            .Set("triangles", static_cast<int64_t>(submesh.index_count / 3)));
  }
  return Json::Object()
      .Set("index", index)
      .Set("name", mesh.name)
      .Set("node", mesh.node)
      .Set("vertices", static_cast<int64_t>(mesh.vertices.size()))
      .Set("triangles", static_cast<int64_t>(mesh.indices.size() / 3))
      .Set("bounds", BoundsJson(mesh.bounds))
      .Set("submeshes", std::move(submeshes));
}

Json ClipJson(const animation::Clip& clip, int index) {
  Json events = Json::Array();
  for (const animation::ClipEvent& event : clip.events) {
    events.Push(Json::Object()
                    .Set("name", event.name)
                    .Set("time", static_cast<double>(event.time)));
  }
  return Json::Object()
      .Set("index", index)
      .Set("name", clip.name)
      .Set("duration", static_cast<double>(clip.duration))
      .Set("loop", clip.loop)
      .Set("channels", static_cast<int64_t>(clip.channels.size()))
      .Set("events", std::move(events));
}

Json PhysicsJson(const prefab::Physics& physics) {
  const auto kind = static_cast<size_t>(physics.kind);
  Json json =
      Json::Object()
          .Set("collider",
               kind < kColliderNames.size() ? kColliderNames[kind] : "unknown")
          .Set("center", Vec3Json(physics.center))
          .Set("rotation", QuatJson(physics.rotation))
          .Set("dynamic", physics.dynamic)
          .Set("mass", static_cast<double>(physics.mass))
          .Set("friction", static_cast<double>(physics.friction))
          .Set("restitution", static_cast<double>(physics.restitution))
          .Set("cooked_shape_bytes",
               static_cast<int64_t>(physics.cooked_shape.size()));
  switch (physics.kind) {
    case prefab::ColliderKind::kBox:
      json.Set("half_extents", Vec3Json(physics.half_extents));
      break;
    case prefab::ColliderKind::kSphere:
      json.Set("radius", static_cast<double>(physics.radius));
      break;
    case prefab::ColliderKind::kCapsule:
      json.Set("radius", static_cast<double>(physics.radius))
          .Set("half_height", static_cast<double>(physics.half_height));
      break;
    default:
      break;
  }
  return json;
}

Json SummaryJson(const prefab::Prefab& prefab) {
  const PrefabSummary s = Summarize(prefab);
  return Json::Object()
      .Set("nodes", s.node_count)
      .Set("meshes", s.mesh_count)
      .Set("submeshes", s.submesh_count)
      .Set("vertices", static_cast<int64_t>(s.vertex_count))
      .Set("triangles", static_cast<int64_t>(s.triangle_count))
      .Set("materials", s.material_count)
      .Set("textures", s.texture_count)
      .Set("clips", s.clip_count)
      .Set("sockets", s.socket_count)
      .Set("lights", s.light_count)
      .Set("children", s.child_count)
      .Set("vertex_bytes", static_cast<int64_t>(s.vertex_bytes))
      .Set("index_bytes", static_cast<int64_t>(s.index_bytes))
      .Set("texture_bytes", static_cast<int64_t>(s.texture_bytes))
      .Set("collision_bytes", static_cast<int64_t>(s.collision_bytes));
}

}  // namespace

Json Vec2Json(const glm::vec2& v) {
  return Json::Array().Push(Json::Number(v.x)).Push(Json::Number(v.y));
}

Json Vec3Json(const glm::vec3& v) {
  return Json::Array()
      .Push(Json::Number(v.x))
      .Push(Json::Number(v.y))
      .Push(Json::Number(v.z));
}

Json Vec4Json(const glm::vec4& v) {
  return Json::Array()
      .Push(Json::Number(v.x))
      .Push(Json::Number(v.y))
      .Push(Json::Number(v.z))
      .Push(Json::Number(v.w));
}

Json TransformJson(const scene::Transform& transform) {
  return Json::Object()
      .Set("position", Vec3Json(transform.position))
      .Set("rotation", QuatJson(transform.rotation))
      .Set("rotation_deg", EulerDegreesJson(transform.rotation))
      .Set("scale", Vec3Json(transform.scale));
}

Json TransformJson(const animation::Transform& transform) {
  return Json::Object()
      .Set("position", Vec3Json(transform.translation))
      .Set("rotation", QuatJson(transform.rotation))
      .Set("rotation_deg", EulerDegreesJson(transform.rotation))
      .Set("scale", Vec3Json(transform.scale));
}

Json SceneToJson(const scene::SceneData& scene, const std::string& asset_root) {
  Json prefabs = Json::Array();
  for (const std::string& path : scene.prefabs) {
    prefabs.Push(Json::String(DisplayPath(path, asset_root)));
  }
  Json instances = Json::Array();
  for (const scene::Instance& instance : scene.instances) {
    instances.Push(InstanceJson(scene, instance, asset_root));
  }
  Json lights = Json::Array();
  for (size_t i = 0; i < scene.point_lights.size(); ++i) {
    const scene::ScenePointLight& light = scene.point_lights[i];
    lights.Push(Json::Object()
                    .Set("index", static_cast<int64_t>(i))
                    .Set("name", light.name)
                    .Set("position", Vec3Json(light.position))
                    .Set("color", Vec3Json(light.color))
                    .Set("intensity", static_cast<double>(light.intensity))
                    .Set("range", static_cast<double>(light.range)));
  }
  const scene::Environment& env = scene.environment;
  const scene::PlayerStart& start = scene.player_start;
  return Json::Object()
      .Set("prefabs", std::move(prefabs))
      .Set("instances", std::move(instances))
      .Set("point_lights", std::move(lights))
      .Set("sun", SunJson(scene.sun))
      .Set("environment",
           Json::Object()
               .Set("hdri",
                    env.hdri_path.empty()
                        ? Json::Null()
                        : Json::String(DisplayPath(env.hdri_path, asset_root)))
               .Set("intensity", static_cast<double>(env.intensity))
               .Set("rotation_deg",
                    static_cast<double>(glm::degrees(env.rotation_radians)))
               .Set("exposure_ev", static_cast<double>(env.exposure_ev)))
      .Set("player_start",
           Json::Object()
               .Set("position", Vec3Json(start.position))
               .Set("yaw_deg",
                    static_cast<double>(glm::degrees(start.yaw_radians)))
               .Set("pitch_deg",
                    static_cast<double>(glm::degrees(start.pitch_radians))))
      .Set("gravity", Vec3Json(scene.gravity));
}

Json PrefabToJson(const prefab::Prefab& prefab, const std::string& asset_root) {
  Json nodes = Json::Array();
  for (size_t i = 0; i < prefab.hierarchy.nodes.size(); ++i) {
    const animation::Node& node = prefab.hierarchy.nodes[i];
    nodes.Push(Json::Object()
                   .Set("index", static_cast<int64_t>(i))
                   .Set("name", node.name)
                   .Set("parent", node.parent)
                   .Set("rest", TransformJson(node.rest)));
  }
  Json meshes = Json::Array();
  for (size_t i = 0; i < prefab.meshes.size(); ++i) {
    meshes.Push(MeshJson(prefab.meshes[i], static_cast<int>(i)));
  }
  Json materials = Json::Array();
  for (size_t i = 0; i < prefab.materials.size(); ++i) {
    materials.Push(
        MaterialJson(prefab, prefab.materials[i], static_cast<int>(i)));
  }
  Json textures = Json::Array();
  for (size_t i = 0; i < prefab.textures.size(); ++i) {
    const prefab::Texture& texture = prefab.textures[i];
    textures.Push(
        Json::Object()
            .Set("index", static_cast<int64_t>(i))
            .Set("name", texture.name)
            .Set("format", FormatLabel(texture.format))
            .Set("width", texture.width)
            .Set("height", texture.height)
            .Set("mips", static_cast<int64_t>(texture.mips.size()))
            .Set("bytes", static_cast<int64_t>(TextureByteSize(texture))));
  }
  Json clips = Json::Array();
  for (size_t i = 0; i < prefab.clips.size(); ++i) {
    clips.Push(ClipJson(prefab.clips[i], static_cast<int>(i)));
  }
  Json sockets = Json::Array();
  for (size_t i = 0; i < prefab.sockets.size(); ++i) {
    const prefab::Socket& socket = prefab.sockets[i];
    sockets.Push(Json::Object()
                     .Set("index", static_cast<int64_t>(i))
                     .Set("name", socket.name)
                     .Set("node", socket.node)
                     .Set("local", TransformJson(socket.local)));
  }
  // In file order, as an object (keys are unique).
  Json tags = Json::Object();
  for (const prefab::Tag& tag : prefab.tags) {
    tags.Set(tag.key, tag.value);
  }
  Json lights = Json::Array();
  for (size_t i = 0; i < prefab.lights.size(); ++i) {
    const prefab::PointLight& light = prefab.lights[i];
    lights.Push(Json::Object()
                    .Set("index", static_cast<int64_t>(i))
                    .Set("name", light.name)
                    .Set("node", light.node)
                    .Set("position", Vec3Json(light.position))
                    .Set("color", Vec3Json(light.color))
                    .Set("intensity", static_cast<double>(light.intensity))
                    .Set("range", static_cast<double>(light.range)));
  }
  Json children = Json::Array();
  for (size_t i = 0; i < prefab.children.size(); ++i) {
    const prefab::ChildPrefab& child = prefab.children[i];
    children.Push(Json::Object()
                      .Set("index", static_cast<int64_t>(i))
                      .Set("path", DisplayPath(child.path, asset_root))
                      .Set("node", child.node)
                      .Set("local", TransformJson(child.local)));
  }
  return Json::Object()
      .Set("summary", SummaryJson(prefab))
      .Set("bounds", BoundsJson(prefab.bounds))
      .Set("nodes", std::move(nodes))
      .Set("meshes", std::move(meshes))
      .Set("materials", std::move(materials))
      .Set("textures", std::move(textures))
      .Set("clips", std::move(clips))
      .Set("sockets", std::move(sockets))
      .Set("tags", std::move(tags))
      .Set("lights", std::move(lights))
      .Set("children", std::move(children))
      .Set("physics", PhysicsJson(prefab.physics))
      .Set("source_model", prefab.edit.source_model.empty()
                               ? Json::Null()
                               : Json::String(DisplayPath(
                                     prefab.edit.source_model, asset_root)));
}

}  // namespace blue::editor
