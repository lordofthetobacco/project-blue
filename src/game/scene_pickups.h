#ifndef BLUE_GAME_SCENE_PICKUPS_H_
#define BLUE_GAME_SCENE_PICKUPS_H_

#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include <glm/mat4x4.hpp>

#include "asset/mesh_data.h"
#include "interact/interaction_focus.h"
#include "scene/scene_data.h"
#include "weapon/weapon_def.h"
#include "weapon/weapon_item.h"
#include "world/prefab_cache.h"
#include "world/world.h"
#include "world/world_physics.h"

// Weapons placed in a scene as pick-ups: the glue between the running world
// (instances, their bodies), the weapon library and the interaction focus.
// Lives on the game side because it needs all three.

namespace blue::game {

// Returns the box `bounds` (prefab space) placed by `matrix` (prefab space
// -> world, possibly scaled), for aiming at an item.
[[nodiscard]] interact::OrientedBox PlacedBox(const glm::mat4& matrix,
                                              const asset::Bounds& bounds);

// One scene instance the player can pick up.
struct ScenePickup {
  scene::InstanceId instance = scene::kNoInstance;
  // What picking it up gives: a preset's build when one is named like its
  // `weapon` tag, else its own single-piece definition (the game decides).
  weapon::WeaponDefId def{};
  size_t placement = 0;  // its own prefab's, in World::placements()
  // Its own prefab's bounds: it lies in the scene as that prefab.
  asset::Bounds bounds;
  bool taken = false;  // picked up: gone until the next Find()
};

// The weapon pick-ups of one world build. A pick-up is a scene instance at
// the scene root whose own prefab is a weapon definition; a weapon instance
// nested under another instance is not one (logged). Taking one hides the
// instance and its descendants in the running world (World::HideInstance)
// and removes their bodies; the scene data is never changed, so the next
// world build (Find()) brings it back.
//
// Allocates in Find() only. Not thread-safe.
class ScenePickups {
 public:
  // Returns the weapon definition of a prefab, or nullopt if it is not a
  // weapon (the game registers it in its library on first sight).
  using DefinitionOf =
      std::function<std::optional<weapon::WeaponDefId>(world::PrefabId)>;

  ScenePickups() = default;
  ScenePickups(const ScenePickups&) = delete;
  ScenePickups& operator=(const ScenePickups&) = delete;

  // Finds the pick-ups of `world`, just built from `scene`.
  void Find(const scene::SceneData& scene, const world::World& world,
            const DefinitionOf& definition_of);

  // Appends a kSceneWeapon candidate (id = the instance) per pick-up not
  // taken, shaped by its prefab's bounds at its placement's current world
  // matrix (which follows a dynamic body). Does not allocate once `out` has
  // grown.
  void AppendCandidates(const world::World& world,
                        std::vector<interact::Interactable>& out) const;

  // Takes pick-up `instance` out of the running world and returns a fresh
  // weapon of its definition (weapon::MakeFreshWeapon); nullopt for an
  // unknown or taken one. Costs one pass over the world's instances and
  // bodies, no allocation.
  std::optional<weapon::WeaponItem> Take(scene::InstanceId instance,
                                         world::World& world,
                                         world::WorldPhysics& physics,
                                         const weapon::WeaponLibrary& library);

  [[nodiscard]] std::span<const ScenePickup> pickups() const {
    return pickups_;
  }

 private:
  std::vector<ScenePickup> pickups_;
};

}  // namespace blue::game

#endif  // BLUE_GAME_SCENE_PICKUPS_H_
