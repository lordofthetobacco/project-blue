#ifndef BLUE_EDITOR_SCENE_MODE_SCENE_EDIT_H_
#define BLUE_EDITOR_SCENE_MODE_SCENE_EDIT_H_

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <glm/vec3.hpp>

#include "scene/scene_data.h"

// Editing operations of the editor's Scene mode that need neither a GPU nor
// ImGui, so they are unit-tested. They work on a scene::SceneData (the
// document's state inside a Document::Edit) and keep it consistent:
// scene::ValidateSceneData() holds afterwards if it held before.

namespace blue::editor {

// How far in front of the camera a new instance goes when the view ray does
// not meet the ground (looking up, or the ground is far away).
inline constexpr float kPlacementDistance = 5.0f;

// Returns where an instance dropped along the ray from `origin` in
// `direction` (unit length) lands: where the ray meets the ground plane
// y = 0 if that is within `max_distance`, otherwise `fallback_distance`
// along the ray. `hit_distance`, when set, is the distance to geometry
// picked along the ray (it wins when it is nearer than the ground).
[[nodiscard]] glm::vec3 PlacementPoint(
    const glm::vec3& origin, const glm::vec3& direction,
    std::optional<float> hit_distance = std::nullopt,
    float max_distance = 200.0f, float fallback_distance = kPlacementDistance);

// Returns the name a new instance of `prefab_path` gets: the file's stem,
// made unique among the scene's instance names ("crate", "crate 2", ...).
[[nodiscard]] std::string InstanceNameFor(const scene::SceneData& scene,
                                          std::string_view prefab_path);

// Adds an instance of the prefab at `prefab_path` (added to the prefab table
// if needed) at `position` under `parent` (kNoInstance = root; the position
// is then converted into the parent's space), named by InstanceNameFor().
// Returns its id, or kNoInstance if `prefab_path` is empty or `parent` does
// not exist (scene unchanged).
scene::InstanceId PlacePrefab(scene::SceneData& scene,
                              std::string_view prefab_path,
                              const glm::vec3& position,
                              scene::InstanceId parent = scene::kNoInstance);

// Removes the instances `ids` (with their descendants) and compacts the
// prefab table. Missing ids are ignored. Returns how many listed instances
// were removed.
int DeleteInstances(scene::SceneData& scene,
                    std::span<const scene::InstanceId> ids);

// Duplicates the instances `ids` (with their descendants; an id whose
// ancestor is also listed is skipped, it is copied with the ancestor). Copies
// are renamed to unique names and moved by `offset` (world space, applied to
// each copied root). Returns the ids of the copied roots, in the order of
// `ids`.
std::vector<scene::InstanceId> DuplicateInstances(
    scene::SceneData& scene, std::span<const scene::InstanceId> ids,
    const glm::vec3& offset = glm::vec3(0.0f));

// One row of the outliner: an instance and its depth in the forest.
struct OutlinerRow {
  scene::InstanceId id = scene::kNoInstance;
  int depth = 0;
  bool has_children = false;
};

// Returns the instance forest in display order: each root followed by its
// subtree, depth first; siblings keep their order in `scene.instances`.
// Instances below a collapsed id (listed in `collapsed`) are left out.
// Instances in a parent cycle (not valid scenes) are not listed.
[[nodiscard]] std::vector<OutlinerRow> OutlinerRows(
    const scene::SceneData& scene,
    std::span<const scene::InstanceId> collapsed = {});

// Returns the override of `instance` for `material_name`, adding an empty
// one (no replacement, no tweaks) if there is none.
scene::MaterialOverride& FindOrAddMaterialOverride(
    scene::Instance& instance, std::string_view material_name);

// Removes the override of `instance` for `material_name` and then compacts
// the prefab table of `scene` (the override may have been the last user of a
// material library). Returns false if there was none.
bool RemoveMaterialOverride(scene::SceneData& scene, scene::InstanceId id,
                            std::string_view material_name);

// Makes the override of instance `id` for `material_name` replace it by
// `source_material` from the prefab at `source_prefab_path` (added to the
// prefab table if needed), keeping its parameter tweaks. An empty path makes
// it parameter-only again. Returns false if the instance does not exist or
// `source_material` is empty while a path is given.
bool SetMaterialReplacement(scene::SceneData& scene, scene::InstanceId id,
                            std::string_view material_name,
                            std::string_view source_prefab_path,
                            std::string source_material);

// Returns the override if it changes nothing (no replacement, no tweaks):
// such overrides can be dropped.
[[nodiscard]] bool IsEmptyOverride(const scene::MaterialOverride& material);

// Returns the instance id that follows (`step` = +1) or precedes (-1)
// `current` in outliner order, wrapping around; the first row's id when
// `current` is not listed; kNoInstance for an empty scene. Used by the
// "select next / previous" shortcuts.
[[nodiscard]] scene::InstanceId StepInstance(const scene::SceneData& scene,
                                             scene::InstanceId current,
                                             int step);

// Returns the instance whose name equals `name` (first in outliner order),
// or kNoInstance. Used by text commands ("select-instance <name>").
[[nodiscard]] scene::InstanceId FindInstanceByName(
    const scene::SceneData& scene, std::string_view name);

}  // namespace blue::editor

#endif  // BLUE_EDITOR_SCENE_MODE_SCENE_EDIT_H_
