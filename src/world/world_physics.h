#ifndef BLUE_WORLD_WORLD_PHYSICS_H_
#define BLUE_WORLD_WORLD_PHYSICS_H_

#include <memory>
#include <optional>

#include "physics/physics_world.h"
#include "scene/scene_data.h"
#include "world/core/collision_world.h"
#include "world/world.h"

namespace blue::world {

// Bodies for a World: one per placement whose prefab has a collider (and
// whose instance does not disable physics), from the prefab's cooked shape
// (restored once per prefab) or, without one, its primitive parameters,
// scaled by the placement's scale. Instance physics overrides (motion, mass,
// friction, restitution, disabled) apply to the instance's own prefab and
// its nested prefabs. Optional: the editor can show a World without one.
// A thin wrapper over CollisionWorld, which the headless simulation uses
// directly, so both build identical bodies.
//
// Borrows `physics`, which must outlive it and should get bodies from no one
// else. Must be destroyed before the PhysicsRuntime. Not thread-safe;
// neither copyable nor movable.
class WorldPhysics {
 public:
  explicit WorldPhysics(physics::PhysicsWorld& physics);
  WorldPhysics(const WorldPhysics&) = delete;
  WorldPhysics& operator=(const WorldPhysics&) = delete;
  ~WorldPhysics();

  // Removes every body, sets the gravity of `scene` and creates the bodies
  // of `world`, which must have been built from `scene`. `options` decide
  // what becomes of dynamic bodies, nested prefabs' included: simulated (the
  // default: offline play) or kinematic (a networked client, which moves
  // them to the server's poses with PhysicsWorld::MoveKinematic).
  PhysicsBuildSummary Build(const scene::SceneData& scene, const World& world,
                            CollisionWorldOptions options = {});

  // Removes the bodies of the instances `world` hides (World::HideInstance)
  // and returns how many were removed. One pass over the bodies, no
  // allocation; the entries stay (marked removed) until the next Build().
  int RemoveHiddenBodies(const World& world);

  // Writes the interpolated poses of the dynamic bodies (simulated or
  // kinematic) into `world` (as the world matrix of their instance's prefab,
  // keeping its scale). Bodies of nested prefabs are not synced (they stay
  // with their parent).
  void SyncToWorld(World& world) const;

  // Returns the instance owning `body`, or nullopt.
  [[nodiscard]] std::optional<scene::InstanceId> InstanceFor(
      physics::BodyId body) const;
  // Returns the body of instance `id`'s own prefab, or nullopt.
  [[nodiscard]] std::optional<physics::BodyId> BodyFor(
      scene::InstanceId id) const;

 private:
  physics::PhysicsWorld& physics_;
  CollisionWorldOptions options_;
  // Recreated when a Build() asks for other options (its options are fixed
  // at construction).
  std::unique_ptr<CollisionWorld> collision_;
};

}  // namespace blue::world

#endif  // BLUE_WORLD_WORLD_PHYSICS_H_
