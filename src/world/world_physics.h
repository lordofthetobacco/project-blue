#ifndef BLUE_WORLD_WORLD_PHYSICS_H_
#define BLUE_WORLD_WORLD_PHYSICS_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

#include "physics/physics_world.h"
#include "physics/shape_cooking.h"
#include "scene/scene_data.h"
#include "world/world.h"

namespace blue::world {

struct PhysicsBuildSummary {
  int created = 0;  // bodies created
  int failed = 0;   // placements with a collider that got no body (logged)
};

// Bodies for a World: one per placement whose prefab has a collider (and
// whose instance does not disable physics), from the prefab's cooked shape
// (restored once per prefab) or, without one, its primitive parameters,
// scaled by the placement's scale. Instance physics overrides (motion, mass,
// friction, restitution, disabled) apply to the instance's own prefab and
// its nested prefabs. Optional: the editor can show a World without one.
//
// Borrows `physics`, which must outlive it and should get bodies from no one
// else. Must be destroyed before the PhysicsRuntime. Not thread-safe;
// neither copyable nor movable.
class WorldPhysics {
 public:
  explicit WorldPhysics(physics::PhysicsWorld& physics) : physics_(physics) {}
  WorldPhysics(const WorldPhysics&) = delete;
  WorldPhysics& operator=(const WorldPhysics&) = delete;

  // Removes every body, sets the gravity of `scene` and creates the bodies
  // of `world`, which must have been built from `scene`.
  PhysicsBuildSummary Build(const scene::SceneData& scene, const World& world);

  // Removes the bodies of the instances `world` hides (World::HideInstance)
  // and returns how many were removed. One pass over the bodies, no
  // allocation; the entries stay (marked removed) until the next Build().
  int RemoveHiddenBodies(const World& world);

  // Writes the interpolated poses of the dynamic bodies into `world` (as the
  // world matrix of their instance's prefab, keeping its scale). Bodies of
  // nested prefabs are not synced (they stay with their parent).
  void SyncToWorld(World& world) const;

  // Returns the instance owning `body`, or nullopt.
  [[nodiscard]] std::optional<scene::InstanceId> InstanceFor(
      physics::BodyId body) const;
  // Returns the body of instance `id`'s own prefab, or nullopt.
  [[nodiscard]] std::optional<physics::BodyId> BodyFor(
      scene::InstanceId id) const;

 private:
  struct BodyEntry {
    physics::BodyId body{};
    scene::InstanceId instance = scene::kNoInstance;
    size_t placement = 0;
    glm::vec3 scale{1.0f};
    bool dynamic = false;
    bool root = false;     // the instance's own prefab
    bool removed = false;  // by RemoveHiddenBodies(): `body` is stale
  };

  physics::PhysicsWorld& physics_;
  std::vector<BodyEntry> bodies_;
  std::unordered_map<uint32_t, size_t> by_body_;  // BodyId value -> entry
  // Shapes restored from prefabs, by PrefabId value; shared by the
  // placements of one Build() and cleared by the next.
  std::unordered_map<uint32_t, physics::CookedShape> shapes_;
};

}  // namespace blue::world

#endif  // BLUE_WORLD_WORLD_PHYSICS_H_
