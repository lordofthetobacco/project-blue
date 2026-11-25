#include "world/core/placement_plan.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <glm/mat4x4.hpp>

#include "animation/hierarchy.h"
#include "animation/transform.h"
#include "prefab/prefab.h"
#include "prefab/prefab_file.h"
#include "scene/scene_data.h"

namespace blue::world {
namespace {

// Returns the instances of `scene` to place, as indices into
// scene.instances: visible ones only, parents first. Instances whose parent
// chain is broken (missing parent or a cycle) or hidden are skipped; a
// broken chain is reported once, on its top instance.
std::vector<size_t> OrderInstances(const scene::SceneData& scene,
                                   std::vector<WorldIssue>& issues) {
  std::unordered_map<scene::InstanceId, size_t> by_id;
  for (size_t i = 0; i < scene.instances.size(); ++i) {
    by_id.emplace(scene.instances[i].id, i);
  }
  // 0 = unvisited, 1 = in progress, 2 = placed, 3 = skipped.
  std::vector<uint8_t> state(scene.instances.size(), 0);
  std::vector<size_t> order;
  order.reserve(scene.instances.size());
  for (size_t root = 0; root < scene.instances.size(); ++root) {
    // Walk up to the first resolved ancestor, then place downwards.
    std::vector<size_t> path;
    size_t current = root;
    bool broken = false;
    while (state[current] == 0) {
      state[current] = 1;
      path.push_back(current);
      const scene::InstanceId parent = scene.instances[current].parent;
      if (parent == scene::kNoInstance) {
        break;
      }
      auto found = by_id.find(parent);
      if (found == by_id.end() || state[found->second] == 1) {
        broken = true;
        break;
      }
      current = found->second;
    }
    // The top of the path decides for the whole path.
    const size_t top = path.empty() ? current : path.back();
    const scene::InstanceId top_parent = scene.instances[top].parent;
    bool skip =
        broken || (top_parent != scene::kNoInstance &&
                   by_id.contains(top_parent) && state[by_id[top_parent]] == 3);
    if (broken && !path.empty()) {
      issues.push_back({scene.instances[path.back()].id,
                        "broken parent chain (missing parent or a cycle)"});
    }
    for (auto it = path.rbegin(); it != path.rend(); ++it) {
      skip = skip || !scene.instances[*it].visible;
      state[*it] = skip ? 3 : 2;
      if (!skip) {
        order.push_back(*it);
      }
    }
  }
  return order;
}

// The state of one ResolvePlacements() call.
class Resolver {
 public:
  Resolver(const scene::SceneData& scene, PrefabSource& prefabs,
           PlacementPlan& plan)
      : scene_(scene), prefabs_(prefabs), plan_(plan) {}

  // Adds the instance at scene.instances[index]; its parent (if any) is
  // already in the plan.
  void AddInstance(size_t index) {
    const scene::Instance& instance = scene_.instances[index];
    PlannedInstance planned{
        .id = instance.id, .parent = instance.parent, .scene_index = index};
    glm::mat4 parent_world(1.0f);
    if (auto found = planned_index_.find(instance.parent);
        found != planned_index_.end()) {
      parent_world = plan_.instances[found->second].world;
    }
    planned.world = parent_world * instance.transform.Matrix();
    if (instance.prefab != scene::kNoPrefab) {
      if (std::optional<PrefabId> prefab =
              ResolveScenePrefab(scene_, prefabs_, instance.id, instance.prefab,
                                 plan_.scene_prefabs, plan_.issues)) {
        planned.first_placement = static_cast<int>(plan_.placements.size());
        std::vector<PrefabId> chain;
        AddPlacement(instance.id, *prefab, -1, -1, glm::mat4(1.0f), 0,
                     planned.world, chain);
        planned.placement_count =
            static_cast<int>(plan_.placements.size()) - planned.first_placement;
      }
    }
    planned.issue_end = plan_.issues.size();
    planned_index_.emplace(planned.id, plan_.instances.size());
    plan_.instances.push_back(planned);
  }

 private:
  // Returns the rest-pose node matrices (prefab space) of `prefab`, computed
  // once per prefab.
  const std::vector<glm::mat4>& RestMatrices(PrefabId prefab) {
    const auto key = static_cast<uint32_t>(prefab);
    if (auto found = rest_.find(key); found != rest_.end()) {
      return found->second;
    }
    const animation::Hierarchy& hierarchy = prefabs_.prefab(prefab).hierarchy;
    std::vector<animation::Transform> local(hierarchy.nodes.size());
    std::vector<glm::mat4> matrices(hierarchy.nodes.size());
    animation::ResetToRest(hierarchy, local);
    animation::ComputeWorldMatrices(hierarchy, local, matrices);
    return rest_.emplace(key, std::move(matrices)).first->second;
  }

  // Returns the rest matrix of node `node` of `prefab` (identity for -1 or
  // an index out of range, as PrefabPose::NodeMatrix()).
  glm::mat4 RestNodeMatrix(PrefabId prefab, int node) {
    const std::vector<glm::mat4>& matrices = RestMatrices(prefab);
    if (node < 0 || static_cast<size_t>(node) >= matrices.size()) {
      return glm::mat4(1.0f);
    }
    return matrices[static_cast<size_t>(node)];
  }

  // Adds a placement of `prefab` at `world` and, depth first, its nested
  // prefabs. `chain` holds the prefabs from the instance's own down to the
  // parent, for cycle detection.
  void AddPlacement(scene::InstanceId owner, PrefabId prefab, int parent,
                    int parent_node, const glm::mat4& local, int depth,
                    const glm::mat4& world, std::vector<PrefabId>& chain) {
    const auto index = static_cast<int>(plan_.placements.size());
    plan_.placements.push_back({.owner = owner,
                                .prefab = prefab,
                                .parent = parent,
                                .depth = depth,
                                .world = world});
    plan_.attachments.push_back({.parent_node = parent_node, .local = local});
    chain.push_back(prefab);
    // Copy: the source's vectors stay put, but be robust to Get() appending.
    const std::vector<prefab::ChildPrefab> children =
        prefabs_.prefab(prefab).children;
    for (const prefab::ChildPrefab& child : children) {
      auto child_id = prefabs_.Get(child.path);
      if (!child_id.ok()) {
        Issue(owner, std::format("cannot load nested prefab {}: {}", child.path,
                                 prefab::ToString(child_id.error())));
        continue;
      }
      if (std::ranges::find(chain, child_id.value()) != chain.end()) {
        std::string cycle;
        for (const PrefabId link : chain) {
          cycle += prefabs_.key(link) + " -> ";
        }
        Issue(owner, "nested prefab cycle: " + cycle + child.path);
        continue;
      }
      if (depth + 1 > kMaxPrefabDepth) {
        Issue(owner, std::format("nested prefab {} exceeds the depth limit "
                                 "of {}",
                                 child.path, kMaxPrefabDepth));
        continue;
      }
      // Evaluated as World::UpdatePlacementMatrices() does: (parent * node)
      // * local, so both give bit-identical matrices.
      const glm::mat4 child_local = animation::ToMatrix(child.local);
      const glm::mat4 child_world =
          world * RestNodeMatrix(prefab, child.node) * child_local;
      AddPlacement(owner, child_id.value(), index, child.node, child_local,
                   depth + 1, child_world, chain);
    }
    chain.pop_back();
  }

  void Issue(scene::InstanceId id, std::string message) {
    plan_.issues.push_back({id, std::move(message)});
  }

  const scene::SceneData& scene_;
  PrefabSource& prefabs_;
  PlacementPlan& plan_;
  // InstanceId -> index into plan_.instances.
  std::unordered_map<scene::InstanceId, size_t> planned_index_;
  // PrefabId value -> rest node matrices.
  std::unordered_map<uint32_t, std::vector<glm::mat4>> rest_;
};

}  // namespace

std::optional<PrefabId> ResolveScenePrefab(const scene::SceneData& scene,
                                           PrefabSource& prefabs,
                                           scene::InstanceId owner, int index,
                                           ScenePrefabSlots& slots,
                                           std::vector<WorldIssue>& issues) {
  if (index < 0 || static_cast<size_t>(index) >= scene.prefabs.size()) {
    issues.push_back(
        {owner, std::format("prefab index {} out of range", index)});
    return std::nullopt;
  }
  if (slots.size() < scene.prefabs.size()) {
    slots.resize(scene.prefabs.size());
  }
  std::optional<std::optional<PrefabId>>& slot =
      slots[static_cast<size_t>(index)];
  if (slot.has_value()) {
    return *slot;
  }
  const std::string& path = scene.prefabs[static_cast<size_t>(index)];
  auto id = prefabs.Get(path);
  if (!id.ok()) {
    issues.push_back({owner, std::format("cannot load prefab {}: {}", path,
                                         prefab::ToString(id.error()))});
    slot = std::optional<PrefabId>();
    return std::nullopt;
  }
  slot = id.value();
  return id.value();
}

PlacementPlan ResolvePlacements(const scene::SceneData& scene,
                                PrefabSource& prefabs) {
  PlacementPlan plan;
  const std::vector<size_t> order = OrderInstances(scene, plan.issues);
  plan.instances.reserve(order.size());
  Resolver resolver(scene, prefabs, plan);
  for (const size_t index : order) {
    resolver.AddInstance(index);
  }
  return plan;
}

}  // namespace blue::world
