#include "scene/scene_data.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/mat3x3.hpp>
#include <glm/matrix.hpp>

#include "core/path.h"

namespace blue::scene {
namespace {

bool IsFinite(const glm::vec2& v) {
  return std::isfinite(v.x) && std::isfinite(v.y);
}

bool IsFinite(const glm::vec3& v) {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

bool IsFinite(const glm::vec4& v) {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) &&
         std::isfinite(v.w);
}

bool IsFinite(const glm::quat& q) {
  return std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) &&
         std::isfinite(q.w);
}

bool InUnitRange(float value) { return value >= 0.0f && value <= 1.0f; }

bool IsValidPrefabIndex(const SceneData& scene, int index) {
  return index >= 0 && static_cast<size_t>(index) < scene.prefabs.size();
}

// Returns why `material` is invalid within an instance of `scene`, or
// std::nullopt.
std::optional<std::string> ValidateMaterialOverride(
    const SceneData& scene, const MaterialOverride& material) {
  if (material.material_name.empty()) {
    return "material override without a material name";
  }
  const std::string& name = material.material_name;
  if (material.source_prefab == kNoPrefab) {
    if (!material.source_material.empty()) {
      return std::format(
          "material override '{}' names a source material but no prefab", name);
    }
  } else if (!IsValidPrefabIndex(scene, material.source_prefab)) {
    return std::format("material override '{}' has an invalid source prefab",
                       name);
  } else if (material.source_material.empty()) {
    return std::format(
        "material override '{}' has a source prefab but no material", name);
  }
  if (material.base_color.has_value() &&
      !(IsFinite(*material.base_color) && InUnitRange(material.base_color->r) &&
        InUnitRange(material.base_color->g) &&
        InUnitRange(material.base_color->b) &&
        InUnitRange(material.base_color->a))) {
    return std::format("material override '{}': base colour outside [0, 1]",
                       name);
  }
  if ((material.metallic.has_value() && !InUnitRange(*material.metallic)) ||
      (material.roughness.has_value() && !InUnitRange(*material.roughness))) {
    return std::format(
        "material override '{}': metallic or roughness outside [0, 1]", name);
  }
  if (material.emissive.has_value() &&
      !(IsFinite(*material.emissive) && material.emissive->r >= 0.0f &&
        material.emissive->g >= 0.0f && material.emissive->b >= 0.0f)) {
    return std::format(
        "material override '{}': negative or non-finite "
        "emissive colour",
        name);
  }
  if (material.emissive_strength.has_value() &&
      !(std::isfinite(*material.emissive_strength) &&
        *material.emissive_strength >= 0.0f)) {
    return std::format(
        "material override '{}': negative or non-finite emissive strength",
        name);
  }
  if (material.uv_scale.has_value() &&
      !(IsFinite(*material.uv_scale) && material.uv_scale->x > 0.0f &&
        material.uv_scale->y > 0.0f)) {
    return std::format("material override '{}': uv scale must be > 0", name);
  }
  return std::nullopt;
}

// Returns why `physics` is invalid, or std::nullopt.
std::optional<std::string> ValidatePhysicsOverride(
    const PhysicsOverride& physics) {
  if (physics.motion.has_value() && *physics.motion != MotionType::kStatic &&
      *physics.motion != MotionType::kDynamic) {
    return "physics override: unknown motion type";
  }
  if (physics.mass.has_value() &&
      !(std::isfinite(*physics.mass) && *physics.mass > 0.0f)) {
    return "physics override: mass must be > 0";
  }
  if (physics.friction.has_value() &&
      !(std::isfinite(*physics.friction) && *physics.friction >= 0.0f)) {
    return "physics override: friction must be >= 0";
  }
  if (physics.restitution.has_value() && !InUnitRange(*physics.restitution)) {
    return "physics override: restitution outside [0, 1]";
  }
  return std::nullopt;
}

// Returns why `instance` is invalid within `scene` (parents excluded, see
// ValidateHierarchy()), or std::nullopt.
std::optional<std::string> ValidateInstance(const SceneData& scene,
                                            const Instance& instance) {
  const auto describe = [&](std::string_view what) {
    return std::format("instance {} '{}': {}", instance.id, instance.name,
                       what);
  };
  if (instance.prefab != kNoPrefab &&
      !IsValidPrefabIndex(scene, instance.prefab)) {
    return describe("invalid prefab index");
  }
  if (!IsFinite(instance.transform.position) ||
      !IsFinite(instance.transform.rotation) ||
      !IsFinite(instance.transform.scale)) {
    return describe("non-finite transform");
  }
  std::unordered_set<std::string_view> names;
  for (const MaterialOverride& material : instance.materials) {
    if (auto error = ValidateMaterialOverride(scene, material)) {
      return describe(*error);
    }
    if (!names.insert(material.material_name).second) {
      return describe(std::format("material '{}' overridden twice",
                                  material.material_name));
    }
  }
  if (instance.physics.has_value()) {
    if (auto error = ValidatePhysicsOverride(*instance.physics)) {
      return describe(*error);
    }
  }
  return std::nullopt;
}

// Returns why the parent links of `scene` are invalid (unknown parent or a
// cycle), or std::nullopt. Ids must already be known to be unique.
std::optional<std::string> ValidateHierarchy(const SceneData& scene) {
  std::unordered_map<InstanceId, InstanceId> parent_of;
  parent_of.reserve(scene.instances.size());
  for (const Instance& instance : scene.instances) {
    parent_of.emplace(instance.id, instance.parent);
  }
  for (const Instance& instance : scene.instances) {
    if (instance.parent != kNoInstance &&
        !parent_of.contains(instance.parent)) {
      return std::format("instance {} '{}': parent {} does not exist",
                         instance.id, instance.name, instance.parent);
    }
  }
  // A chain longer than the instance count must revisit an instance.
  for (const Instance& instance : scene.instances) {
    InstanceId current = instance.parent;
    for (size_t steps = 0; current != kNoInstance; ++steps) {
      if (steps >= scene.instances.size()) {
        return std::format("instance {} '{}': parent cycle", instance.id,
                           instance.name);
      }
      current = parent_of.at(current);
    }
  }
  return std::nullopt;
}

// Returns the ids of `root` and all its descendants, `root` first.
std::vector<InstanceId> Subtree(const SceneData& scene, InstanceId root) {
  std::vector<InstanceId> ids = {root};
  // Breadth-first over the flat list; ValidateSceneData() guarantees no
  // cycles, and a cycle could not grow `ids` past the instance count.
  for (size_t i = 0; i < ids.size() && ids.size() <= scene.instances.size();
       ++i) {
    for (const Instance& instance : scene.instances) {
      if (instance.parent == ids[i] && instance.id != root) {
        ids.push_back(instance.id);
      }
    }
  }
  return ids;
}

bool SameTransform(const Transform& a, const Transform& b) {
  return a.position == b.position && a.rotation == b.rotation &&
         a.scale == b.scale;
}

bool SameSun(const SunLight& a, const SunLight& b) {
  return a.direction == b.direction && a.color == b.color &&
         a.intensity == b.intensity && a.casts_shadows == b.casts_shadows;
}

bool SameEnvironment(const Environment& a, const Environment& b) {
  return a.hdri_path == b.hdri_path && a.intensity == b.intensity &&
         a.rotation_radians == b.rotation_radians &&
         a.exposure_ev == b.exposure_ev;
}

}  // namespace

bool operator==(const Instance& a, const Instance& b) {
  return a.id == b.id && a.name == b.name && a.prefab == b.prefab &&
         a.parent == b.parent && SameTransform(a.transform, b.transform) &&
         a.visible == b.visible && a.materials == b.materials &&
         a.physics == b.physics;
}

bool operator==(const SceneData& a, const SceneData& b) {
  return a.prefabs == b.prefabs && a.instances == b.instances &&
         SameSun(a.sun, b.sun) && a.point_lights == b.point_lights &&
         SameEnvironment(a.environment, b.environment) &&
         a.player_start == b.player_start && a.gravity == b.gravity &&
         a.next_instance_id == b.next_instance_id;
}

int AddPrefab(SceneData& scene, std::string_view path) {
  const std::string normalized = core::NormalizePath(path);
  for (size_t i = 0; i < scene.prefabs.size(); ++i) {
    if (core::NormalizePath(scene.prefabs[i]) == normalized) {
      return static_cast<int>(i);
    }
  }
  scene.prefabs.push_back(normalized);
  return static_cast<int>(scene.prefabs.size() - 1);
}

InstanceId AddInstance(SceneData& scene, Instance instance) {
  if ((instance.prefab != kNoPrefab &&
       !IsValidPrefabIndex(scene, instance.prefab)) ||
      (instance.parent != kNoInstance &&
       FindInstance(scene, instance.parent) == nullptr)) {
    return kNoInstance;
  }
  instance.id = scene.next_instance_id++;
  scene.instances.push_back(std::move(instance));
  return scene.instances.back().id;
}

bool RemoveInstance(SceneData& scene, InstanceId id) {
  if (FindInstance(scene, id) == nullptr) {
    return false;
  }
  const std::vector<InstanceId> doomed = Subtree(scene, id);
  std::erase_if(scene.instances, [&](const Instance& instance) {
    return std::ranges::find(doomed, instance.id) != doomed.end();
  });
  CompactPrefabs(scene);
  return true;
}

InstanceId DuplicateInstance(SceneData& scene, InstanceId id) {
  if (FindInstance(scene, id) == nullptr) {
    return kNoInstance;
  }
  const std::vector<InstanceId> originals = Subtree(scene, id);
  std::unordered_map<InstanceId, InstanceId> copy_of;
  copy_of.reserve(originals.size());
  for (const InstanceId original : originals) {
    copy_of.emplace(original, scene.next_instance_id++);
  }
  // Subtree() lists parents before children, so copies keep that order.
  std::vector<Instance> copies;
  copies.reserve(originals.size());
  for (const InstanceId original : originals) {
    Instance copy = *FindInstance(scene, original);
    copy.id = copy_of.at(original);
    if (original != id) {
      copy.parent = copy_of.at(copy.parent);
    }
    copies.push_back(std::move(copy));
  }
  for (Instance& copy : copies) {
    scene.instances.push_back(std::move(copy));
  }
  return copy_of.at(id);
}

void CompactPrefabs(SceneData& scene) {
  std::vector<bool> used(scene.prefabs.size(), false);
  const auto mark = [&](int index) {
    if (IsValidPrefabIndex(scene, index)) {
      used[static_cast<size_t>(index)] = true;
    }
  };
  for (const Instance& instance : scene.instances) {
    mark(instance.prefab);
    for (const MaterialOverride& material : instance.materials) {
      mark(material.source_prefab);
    }
  }

  std::vector<int> remap(scene.prefabs.size(), kNoPrefab);
  std::vector<std::string> kept;
  for (size_t i = 0; i < scene.prefabs.size(); ++i) {
    if (used[i]) {
      remap[i] = static_cast<int>(kept.size());
      kept.push_back(std::move(scene.prefabs[i]));
    }
  }
  const auto renumber = [&](int& index) {
    if (IsValidPrefabIndex(scene, index)) {
      index = remap[static_cast<size_t>(index)];
    }
  };
  for (Instance& instance : scene.instances) {
    renumber(instance.prefab);
    for (MaterialOverride& material : instance.materials) {
      renumber(material.source_prefab);
    }
  }
  scene.prefabs = std::move(kept);
}

Instance* FindInstance(SceneData& scene, InstanceId id) {
  if (id == kNoInstance) {
    return nullptr;
  }
  const auto it = std::ranges::find(scene.instances, id, &Instance::id);
  return it == scene.instances.end() ? nullptr : &*it;
}

const Instance* FindInstance(const SceneData& scene, InstanceId id) {
  if (id == kNoInstance) {
    return nullptr;
  }
  const auto it = std::ranges::find(scene.instances, id, &Instance::id);
  return it == scene.instances.end() ? nullptr : &*it;
}

std::optional<glm::mat4> WorldMatrix(const SceneData& scene, InstanceId id) {
  const Instance* instance = FindInstance(scene, id);
  if (instance == nullptr) {
    return std::nullopt;
  }
  glm::mat4 world = instance->transform.Matrix();
  for (size_t steps = 0; instance->parent != kNoInstance; ++steps) {
    instance = FindInstance(scene, instance->parent);
    if (instance == nullptr || steps >= scene.instances.size()) {
      return std::nullopt;
    }
    world = instance->transform.Matrix() * world;
  }
  return world;
}

std::vector<InstanceId> ChildrenOf(const SceneData& scene, InstanceId parent) {
  std::vector<InstanceId> children;
  for (const Instance& instance : scene.instances) {
    if (instance.parent == parent) {
      children.push_back(instance.id);
    }
  }
  return children;
}

bool IsSelfOrAncestor(const SceneData& scene, InstanceId ancestor,
                      InstanceId id) {
  // Bounded by the instance count so a corrupt (cyclic) scene terminates.
  for (size_t steps = 0; id != kNoInstance && steps <= scene.instances.size();
       ++steps) {
    if (id == ancestor) {
      return true;
    }
    const Instance* instance = FindInstance(scene, id);
    id = instance == nullptr ? kNoInstance : instance->parent;
  }
  return false;
}

Transform TransformFromMatrix(const glm::mat4& matrix) {
  Transform transform;
  transform.position = glm::vec3(matrix[3]);
  const glm::vec3 x(matrix[0]);
  const glm::vec3 y(matrix[1]);
  const glm::vec3 z(matrix[2]);
  transform.scale = {glm::length(x), glm::length(y), glm::length(z)};
  constexpr float kTiny = 1e-12f;
  if (transform.scale.x < kTiny || transform.scale.y < kTiny ||
      transform.scale.z < kTiny) {
    return transform;  // Degenerate: no meaningful rotation.
  }
  // A mirrored basis is expressed as a negative x scale.
  if (glm::determinant(glm::mat3(x, y, z)) < 0.0f) {
    transform.scale.x = -transform.scale.x;
  }
  const glm::mat3 rotation(x / transform.scale.x, y / transform.scale.y,
                           z / transform.scale.z);
  transform.rotation = glm::normalize(glm::quat_cast(rotation));
  return transform;
}

bool ReparentInstance(SceneData& scene, InstanceId id, InstanceId new_parent,
                      bool keep_world_transform) {
  if (FindInstance(scene, id) == nullptr ||
      (new_parent != kNoInstance &&
       (FindInstance(scene, new_parent) == nullptr ||
        IsSelfOrAncestor(scene, id, new_parent)))) {
    return false;
  }
  if (keep_world_transform) {
    const std::optional<glm::mat4> world = WorldMatrix(scene, id);
    const std::optional<glm::mat4> parent_world =
        new_parent == kNoInstance ? std::optional<glm::mat4>(glm::mat4(1.0f))
                                  : WorldMatrix(scene, new_parent);
    if (!world.has_value() || !parent_world.has_value()) {
      return false;
    }
    FindInstance(scene, id)->transform =
        TransformFromMatrix(glm::inverse(*parent_world) * *world);
  }
  FindInstance(scene, id)->parent = new_parent;
  return true;
}

InstanceId GroupInstances(SceneData& scene, std::span<const InstanceId> ids,
                          std::string name) {
  std::vector<InstanceId> members;
  for (const InstanceId id : ids) {
    if (FindInstance(scene, id) == nullptr ||
        std::ranges::find(members, id) != members.end()) {
      continue;
    }
    // Skip ids whose ancestor is also listed: they move with it.
    const Instance* instance = FindInstance(scene, id);
    const bool nested = std::ranges::any_of(ids, [&](InstanceId other) {
      return other != id && IsSelfOrAncestor(scene, other, instance->parent);
    });
    if (!nested) {
      members.push_back(id);
    }
  }
  if (members.empty()) {
    return kNoInstance;
  }

  const InstanceId first_parent = FindInstance(scene, members[0])->parent;
  const bool shared_parent = std::ranges::all_of(members, [&](InstanceId id) {
    return FindInstance(scene, id)->parent == first_parent;
  });
  const InstanceId parent = shared_parent ? first_parent : kNoInstance;

  glm::vec3 centroid(0.0f);
  for (const InstanceId id : members) {
    centroid += glm::vec3((*WorldMatrix(scene, id))[3]);
  }
  centroid /= static_cast<float>(members.size());

  Instance group;
  group.name = std::move(name);
  group.parent = parent;
  const glm::mat4 parent_world =
      parent == kNoInstance ? glm::mat4(1.0f) : *WorldMatrix(scene, parent);
  group.transform.position =
      glm::vec3(glm::inverse(parent_world) * glm::vec4(centroid, 1.0f));
  const InstanceId group_id = AddInstance(scene, std::move(group));
  for (const InstanceId id : members) {
    ReparentInstance(scene, id, group_id, /*keep_world_transform=*/true);
  }
  return group_id;
}

bool UngroupInstance(SceneData& scene, InstanceId group) {
  const Instance* instance = FindInstance(scene, group);
  if (instance == nullptr) {
    return false;
  }
  const InstanceId parent = instance->parent;
  const bool pure_group = instance->prefab == kNoPrefab;
  for (const InstanceId child : ChildrenOf(scene, group)) {
    ReparentInstance(scene, child, parent, /*keep_world_transform=*/true);
  }
  if (pure_group) {
    RemoveInstance(scene, group);
  }
  return true;
}

SunAngles SunAnglesFromDirection(const glm::vec3& direction) {
  const glm::vec3 towards_sun = -glm::normalize(direction);
  return {std::asin(std::clamp(towards_sun.y, -1.0f, 1.0f)),
          std::atan2(towards_sun.x, towards_sun.z)};
}

glm::vec3 SunDirectionFromAngles(const SunAngles& angles) {
  const float horizontal = std::cos(angles.elevation);
  const glm::vec3 towards_sun(horizontal * std::sin(angles.azimuth),
                              std::sin(angles.elevation),
                              horizontal * std::cos(angles.azimuth));
  return -glm::normalize(towards_sun);
}

const MaterialOverride* FindMaterialOverride(const Instance& instance,
                                             std::string_view material_name) {
  const auto it = std::ranges::find(instance.materials, material_name,
                                    &MaterialOverride::material_name);
  return it == instance.materials.end() ? nullptr : &*it;
}

const MaterialOverride* FindMaterialOverride(const SceneData& scene,
                                             InstanceId id,
                                             std::string_view material_name) {
  const Instance* instance = FindInstance(scene, id);
  return instance == nullptr ? nullptr
                             : FindMaterialOverride(*instance, material_name);
}

std::vector<SceneIssue> ValidateAgainstPrefabs(
    const SceneData& scene,
    std::span<const std::vector<std::string>> material_names) {
  const auto names_of = [&](int prefab) -> const std::vector<std::string>* {
    if (prefab < 0 || static_cast<size_t>(prefab) >= material_names.size()) {
      return nullptr;
    }
    return &material_names[static_cast<size_t>(prefab)];
  };
  const auto contains = [](const std::vector<std::string>& names,
                           std::string_view name) {
    return std::ranges::find(names, name) != names.end();
  };

  std::vector<SceneIssue> issues;
  for (const Instance& instance : scene.instances) {
    const std::vector<std::string>* own = names_of(instance.prefab);
    for (const MaterialOverride& material : instance.materials) {
      if (own != nullptr && !contains(*own, material.material_name)) {
        issues.push_back(
            {instance.id,
             std::format("instance '{}': prefab '{}' has no material '{}'",
                         instance.name,
                         scene.prefabs[static_cast<size_t>(instance.prefab)],
                         material.material_name)});
      }
      const std::vector<std::string>* source = names_of(material.source_prefab);
      if (source != nullptr && !contains(*source, material.source_material)) {
        issues.push_back(
            {instance.id,
             std::format(
                 "instance '{}': prefab '{}' has no material '{}' to "
                 "replace '{}'",
                 instance.name,
                 scene.prefabs[static_cast<size_t>(material.source_prefab)],
                 material.source_material, material.material_name)});
      }
    }
  }
  return issues;
}

std::optional<std::string> ValidateSceneData(const SceneData& scene) {
  std::unordered_set<std::string_view> paths;
  for (const std::string& path : scene.prefabs) {
    if (path.empty()) {
      return "empty prefab path";
    }
    if (!paths.insert(path).second) {
      return std::format("prefab '{}' listed twice", path);
    }
  }

  std::unordered_set<InstanceId> ids;
  ids.reserve(scene.instances.size());
  for (const Instance& instance : scene.instances) {
    if (instance.id == kNoInstance || instance.id >= scene.next_instance_id) {
      return std::format("instance '{}': id {} outside [1, {})", instance.name,
                         instance.id, scene.next_instance_id);
    }
    if (!ids.insert(instance.id).second) {
      return std::format("instance id {} used twice", instance.id);
    }
    if (auto error = ValidateInstance(scene, instance)) {
      return error;
    }
  }
  if (auto error = ValidateHierarchy(scene)) {
    return error;
  }

  for (const ScenePointLight& light : scene.point_lights) {
    if (!IsFinite(light.position) || !IsFinite(light.color) ||
        light.color.r < 0.0f || light.color.g < 0.0f || light.color.b < 0.0f ||
        !std::isfinite(light.intensity) || light.intensity < 0.0f ||
        !std::isfinite(light.range) || light.range <= 0.0f) {
      return std::format("point light '{}': non-finite or out-of-range value",
                         light.name);
    }
  }
  if (auto error = ValidateSunLight(scene.sun)) {
    return std::format("sun: {}", *error);
  }
  if (auto error = ValidateEnvironment(scene.environment)) {
    return std::format("environment: {}", *error);
  }
  if (!IsFinite(scene.player_start.position) ||
      !std::isfinite(scene.player_start.yaw_radians) ||
      !std::isfinite(scene.player_start.pitch_radians)) {
    return "player start: non-finite value";
  }
  if (!IsFinite(scene.gravity)) {
    return "non-finite gravity";
  }
  return std::nullopt;
}

}  // namespace blue::scene
