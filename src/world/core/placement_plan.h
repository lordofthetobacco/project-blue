#ifndef BLUE_WORLD_CORE_PLACEMENT_PLAN_H_
#define BLUE_WORLD_CORE_PLACEMENT_PLAN_H_

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <glm/mat4x4.hpp>

#include "scene/scene_data.h"
#include "world/core/placement.h"
#include "world/core/prefab_id.h"
#include "world/core/prefab_source.h"

// The GPU-free half of building a world: which prefab copies a scene places,
// in which order, where (at rest) and with which problems. World::Build()
// adds poses, material overrides, lights and draws on top; CollisionWorld
// builds the physics bodies from it.

namespace blue::world {

// A problem met while building a world; the affected instance (or nested
// prefab) is skipped, the rest of the world is built.
struct WorldIssue {
  scene::InstanceId instance = scene::kNoInstance;
  std::string message;
};

// Nested prefabs deeper than this are dropped with an issue.
inline constexpr int kMaxPrefabDepth = 16;

// How a placement hangs from the placement it is nested in.
struct PlacementAttachment {
  int parent_node = -1;   // node of the parent placement's prefab
  glm::mat4 local{1.0f};  // relative to that node
};

// One visible scene instance of a plan.
struct PlannedInstance {
  scene::InstanceId id = scene::kNoInstance;
  scene::InstanceId parent = scene::kNoInstance;
  size_t scene_index = 0;  // into SceneData::instances
  glm::mat4 world{1.0f};
  int first_placement = -1;  // its own prefab's placement, or -1
  int placement_count = 0;   // own + nested
  // PlacementPlan::issues[0, issue_end) were met up to and including this
  // instance (lets World interleave its own issues in build order).
  size_t issue_end = 0;
};

// Per SceneData::prefabs entry: nullopt when not looked up yet, then the
// prefab, or nullopt when it could not be loaded (reported once).
using ScenePrefabSlots = std::vector<std::optional<std::optional<PrefabId>>>;

// A resolved scene.
struct PlacementPlan {
  // The visible instances, parents before children.
  std::vector<PlannedInstance> instances;
  // Every placement, depth first per instance; world matrices with every
  // prefab at its rest pose.
  std::vector<Placement> placements;
  std::vector<PlacementAttachment> attachments;  // parallel to placements
  std::vector<WorldIssue> issues;
  ScenePrefabSlots scene_prefabs;
};

// Resolves `scene` through `prefabs` (loaded on demand). Hidden instances and
// their descendants are skipped. Issues: unloadable prefab, nested prefab
// cycle (named path chain), depth limit, broken parent chain (missing parent
// or a cycle). Deterministic: the same scene and prefab files give the same
// plan in every process.
[[nodiscard]] PlacementPlan ResolvePlacements(const scene::SceneData& scene,
                                              PrefabSource& prefabs);

// Returns scene.prefabs[index] loaded through `prefabs`, or nullopt. Looks
// each entry up once (remembered in `slots`); the first failure of an entry
// and an out-of-range index append an issue for `owner`.
[[nodiscard]] std::optional<PrefabId> ResolveScenePrefab(
    const scene::SceneData& scene, PrefabSource& prefabs,
    scene::InstanceId owner, int index, ScenePrefabSlots& slots,
    std::vector<WorldIssue>& issues);

}  // namespace blue::world

#endif  // BLUE_WORLD_CORE_PLACEMENT_PLAN_H_
