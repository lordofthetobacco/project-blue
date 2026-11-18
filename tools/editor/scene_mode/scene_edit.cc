#include "editor/scene_mode/scene_edit.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/matrix.hpp>
#include <glm/vec4.hpp>

#include "core/path.h"
#include "editor/prefab_mode/prefab_edit.h"

namespace blue::editor {
namespace {

std::vector<std::string> InstanceNames(const scene::SceneData& scene) {
  std::vector<std::string> names;
  names.reserve(scene.instances.size());
  for (const scene::Instance& instance : scene.instances) {
    names.push_back(instance.name);
  }
  return names;
}

// Returns `ids` without missing instances, duplicates and instances whose
// ancestor is listed too, in the order of `ids`.
std::vector<scene::InstanceId> SubtreeRoots(
    const scene::SceneData& scene, std::span<const scene::InstanceId> ids) {
  std::vector<scene::InstanceId> roots;
  for (const scene::InstanceId id : ids) {
    const scene::Instance* instance = scene::FindInstance(scene, id);
    if (instance == nullptr || std::ranges::find(roots, id) != roots.end()) {
      continue;
    }
    const bool nested = std::ranges::any_of(ids, [&](scene::InstanceId other) {
      return other != id &&
             scene::IsSelfOrAncestor(scene, other, instance->parent);
    });
    if (!nested) {
      roots.push_back(id);
    }
  }
  return roots;
}

}  // namespace

glm::vec3 PlacementPoint(const glm::vec3& origin, const glm::vec3& direction,
                         std::optional<float> hit_distance, float max_distance,
                         float fallback_distance) {
  std::optional<float> distance = hit_distance;
  // The ray meets y = 0 ahead of the origin only when it heads towards it.
  if (std::abs(direction.y) > 1e-6f) {
    const float ground = -origin.y / direction.y;
    if (ground > 0.0f && ground <= max_distance &&
        (!distance.has_value() || ground < *distance)) {
      distance = ground;
    }
  }
  if (!distance.has_value() || *distance > max_distance) {
    distance = fallback_distance;
  }
  glm::vec3 point = origin + direction * *distance;
  if (std::abs(point.y) < 1e-4f) {
    point.y = 0.0f;  // Exactly on the ground, not a hair above or below.
  }
  return point;
}

std::string InstanceNameFor(const scene::SceneData& scene,
                            std::string_view prefab_path) {
  std::string base = core::FileStem(prefab_path);
  if (base.empty()) {
    base = "instance";
  }
  const std::vector<std::string> taken = InstanceNames(scene);
  return MakeUniqueName(base, taken);
}

scene::InstanceId PlacePrefab(scene::SceneData& scene,
                              std::string_view prefab_path,
                              const glm::vec3& position,
                              scene::InstanceId parent) {
  if (prefab_path.empty()) {
    return scene::kNoInstance;
  }
  glm::mat4 parent_world(1.0f);
  if (parent != scene::kNoInstance) {
    const std::optional<glm::mat4> world = scene::WorldMatrix(scene, parent);
    if (!world.has_value()) {
      return scene::kNoInstance;
    }
    parent_world = *world;
  }
  scene::Instance instance;
  instance.name = InstanceNameFor(scene, prefab_path);
  instance.parent = parent;
  instance.transform.position =
      glm::vec3(glm::inverse(parent_world) * glm::vec4(position, 1.0f));
  instance.prefab = scene::AddPrefab(scene, prefab_path);
  return scene::AddInstance(scene, std::move(instance));
}

int DeleteInstances(scene::SceneData& scene,
                    std::span<const scene::InstanceId> ids) {
  int removed = 0;
  for (const scene::InstanceId id : SubtreeRoots(scene, ids)) {
    if (scene::RemoveInstance(scene, id)) {
      ++removed;
    }
  }
  return removed;
}

std::vector<scene::InstanceId> DuplicateInstances(
    scene::SceneData& scene, std::span<const scene::InstanceId> ids,
    const glm::vec3& offset) {
  std::vector<scene::InstanceId> copies;
  for (const scene::InstanceId id : SubtreeRoots(scene, ids)) {
    const scene::InstanceId copy = scene::DuplicateInstance(scene, id);
    if (copy == scene::kNoInstance) {
      continue;
    }
    // Rename the copied root only; its children keep their names.
    scene::Instance* instance = scene::FindInstance(scene, copy);
    instance->name.clear();
    const std::string base = scene::FindInstance(scene, id)->name;
    instance = scene::FindInstance(scene, copy);
    instance->name = MakeUniqueName(base, InstanceNames(scene));
    if (offset != glm::vec3(0.0f)) {
      const std::optional<glm::mat4> world = scene::WorldMatrix(scene, copy);
      const glm::mat4 parent_world =
          instance->parent == scene::kNoInstance
              ? glm::mat4(1.0f)
              : scene::WorldMatrix(scene, instance->parent)
                    .value_or(glm::mat4(1.0f));
      if (world.has_value()) {
        glm::mat4 moved = *world;
        moved[3] += glm::vec4(offset, 0.0f);
        instance->transform =
            scene::TransformFromMatrix(glm::inverse(parent_world) * moved);
      }
    }
    copies.push_back(copy);
  }
  return copies;
}

std::vector<OutlinerRow> OutlinerRows(
    const scene::SceneData& scene,
    std::span<const scene::InstanceId> collapsed) {
  // Children per parent, in instance order; built once so the walk is
  // linear in the instance count.
  std::unordered_map<scene::InstanceId, std::vector<scene::InstanceId>>
      children;
  children.reserve(scene.instances.size());
  for (const scene::Instance& instance : scene.instances) {
    children[instance.parent].push_back(instance.id);
  }

  std::vector<OutlinerRow> rows;
  rows.reserve(scene.instances.size());
  // Explicit stack (depth-first, siblings in order): pushed in reverse.
  std::vector<OutlinerRow> stack;
  const auto push_children = [&](scene::InstanceId parent, int depth) {
    const auto it = children.find(parent);
    if (it == children.end()) {
      return;
    }
    for (auto child = it->second.rbegin(); child != it->second.rend();
         ++child) {
      stack.push_back({*child, depth, children.contains(*child)});
    }
  };
  push_children(scene::kNoInstance, 0);
  // Bounded by the instance count: a cycle cannot be reached from a root.
  while (!stack.empty() && rows.size() < scene.instances.size()) {
    const OutlinerRow row = stack.back();
    stack.pop_back();
    rows.push_back(row);
    if (std::ranges::find(collapsed, row.id) == collapsed.end()) {
      push_children(row.id, row.depth + 1);
    }
  }
  return rows;
}

scene::MaterialOverride& FindOrAddMaterialOverride(
    scene::Instance& instance, std::string_view material_name) {
  const auto it = std::ranges::find(instance.materials, material_name,
                                    &scene::MaterialOverride::material_name);
  if (it != instance.materials.end()) {
    return *it;
  }
  scene::MaterialOverride added;
  added.material_name = std::string(material_name);
  instance.materials.push_back(std::move(added));
  return instance.materials.back();
}

bool RemoveMaterialOverride(scene::SceneData& scene, scene::InstanceId id,
                            std::string_view material_name) {
  scene::Instance* instance = scene::FindInstance(scene, id);
  if (instance == nullptr) {
    return false;
  }
  const size_t removed = std::erase_if(
      instance->materials, [&](const scene::MaterialOverride& material) {
        return material.material_name == material_name;
      });
  if (removed == 0) {
    return false;
  }
  scene::CompactPrefabs(scene);
  return true;
}

bool SetMaterialReplacement(scene::SceneData& scene, scene::InstanceId id,
                            std::string_view material_name,
                            std::string_view source_prefab_path,
                            std::string source_material) {
  if (scene::FindInstance(scene, id) == nullptr ||
      (!source_prefab_path.empty() && source_material.empty())) {
    return false;
  }
  // AddPrefab may grow the table; take the instance pointer afterwards.
  const int source = source_prefab_path.empty()
                         ? scene::kNoPrefab
                         : scene::AddPrefab(scene, source_prefab_path);
  scene::MaterialOverride& material =
      FindOrAddMaterialOverride(*scene::FindInstance(scene, id), material_name);
  material.source_prefab = source;
  material.source_material =
      source == scene::kNoPrefab ? std::string() : std::move(source_material);
  scene::CompactPrefabs(scene);
  return true;
}

bool IsEmptyOverride(const scene::MaterialOverride& material) {
  return material.source_prefab == scene::kNoPrefab &&
         !material.base_color.has_value() && !material.metallic.has_value() &&
         !material.roughness.has_value() && !material.emissive.has_value() &&
         !material.emissive_strength.has_value() &&
         !material.uv_scale.has_value();
}

scene::InstanceId StepInstance(const scene::SceneData& scene,
                               scene::InstanceId current, int step) {
  const std::vector<OutlinerRow> rows = OutlinerRows(scene);
  if (rows.empty()) {
    return scene::kNoInstance;
  }
  const auto it = std::ranges::find(rows, current, &OutlinerRow::id);
  if (it == rows.end()) {
    return rows.front().id;
  }
  const auto count = static_cast<int>(rows.size());
  const int index = static_cast<int>(it - rows.begin());
  const int next = ((index + step) % count + count) % count;
  return rows[static_cast<size_t>(next)].id;
}

scene::InstanceId FindInstanceByName(const scene::SceneData& scene,
                                     std::string_view name) {
  for (const OutlinerRow& row : OutlinerRows(scene)) {
    if (scene::FindInstance(scene, row.id)->name == name) {
      return row.id;
    }
  }
  return scene::kNoInstance;
}

}  // namespace blue::editor
