#ifndef BLUE_WORLD_CORE_PLACEMENT_H_
#define BLUE_WORLD_CORE_PLACEMENT_H_

#include <glm/mat4x4.hpp>

#include "scene/scene_data.h"
#include "world/core/prefab_id.h"

namespace blue::world {

// One resolved prefab copy: a scene instance's prefab, or a child prefab
// nested in one. `owner` is the scene instance it belongs to.
struct Placement {
  scene::InstanceId owner = scene::kNoInstance;
  PrefabId prefab{};
  // Index into the placement list (World::placements(),
  // PlacementPlan::placements) of the prefab this one is nested in, or -1
  // for the instance's own prefab.
  int parent = -1;
  int depth = 0;          // 0 for the instance's own prefab
  glm::mat4 world{1.0f};  // prefab space -> world
};

}  // namespace blue::world

#endif  // BLUE_WORLD_CORE_PLACEMENT_H_
