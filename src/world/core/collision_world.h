#ifndef BLUE_WORLD_CORE_COLLISION_WORLD_H_
#define BLUE_WORLD_CORE_COLLISION_WORLD_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

#include <glm/vec3.hpp>

#include "physics/physics_world.h"
#include "physics/shape_cooking.h"
#include "scene/scene.h"
#include "scene/scene_data.h"
#include "world/core/placement.h"
#include "world/core/prefab_source.h"

namespace blue::world {

struct PhysicsBuildSummary {
  int created = 0;  // bodies created
  int failed = 0;   // placements with a collider that got no body (logged)
};

// What becomes of bodies whose prefab or instance asks for a dynamic one.
enum class DynamicBodies {
  kSimulated,  // dynamic bodies, moved by the simulation (offline, server)
  // Kinematic bodies, for a networked client that moves them to the
  // server's poses (PhysicsWorld::MoveKinematic) instead of simulating them;
  // they still push the local player and dynamic bodies.
  kKinematic,
  // Static bodies at their placement (nothing moves them).
  kStatic,
};

struct CollisionWorldOptions {
  DynamicBodies dynamic_bodies = DynamicBodies::kSimulated;
};

// One body of a CollisionWorld.
struct CollisionBody {
  physics::BodyId body{};
  scene::InstanceId instance = scene::kNoInstance;
  size_t placement = 0;  // index into the placements it was built from
  glm::vec3 scale{1.0f};
  // The motion the prefab and instance ask for, whatever the options made
  // of it.
  scene::MotionType motion = scene::MotionType::kStatic;
  bool simulated = false;  // a dynamic body the simulation moves
  bool root = false;       // the instance's own prefab
  bool removed = false;    // by RemoveInstanceBodies(): `body` is stale
};

// The physics bodies of a resolved scene: one per placement whose prefab has
// a collider (and whose instance does not disable physics), from the
// prefab's cooked shape (restored once per prefab) or, without one, its
// primitive parameters, scaled by the placement's scale. Instance physics
// overrides (motion, mass, friction, restitution, disabled) apply to the
// instance's own prefab and its nested prefabs. GPU-free: the runtime world
// (WorldPhysics) and the dedicated server build identical bodies with it.
//
// Borrows `physics`, which must outlive it and should get bodies from no one
// else. Must be destroyed before the PhysicsRuntime. Not thread-safe;
// neither copyable nor movable.
class CollisionWorld {
 public:
  explicit CollisionWorld(physics::PhysicsWorld& physics,
                          CollisionWorldOptions options = {})
      : physics_(physics), options_(options) {}
  CollisionWorld(const CollisionWorld&) = delete;
  CollisionWorld& operator=(const CollisionWorld&) = delete;

  // Removes every body, sets the gravity of `scene` and creates the bodies
  // of `placements` (World::placements() or PlacementPlan::placements,
  // resolved from `scene` through `prefabs`), in placement order.
  PhysicsBuildSummary Build(const scene::SceneData& scene,
                            std::span<const Placement> placements,
                            const PrefabSource& prefabs);

  // Removes the bodies of instance `id` (its own prefab's and its nested
  // prefabs', not its child instances') and returns how many were removed.
  // The entries stay (marked removed) until the next Build(). One pass over
  // the bodies, no allocation.
  int RemoveInstanceBodies(scene::InstanceId id);

  // Returns the instance owning `body`, or nullopt.
  [[nodiscard]] std::optional<scene::InstanceId> InstanceFor(
      physics::BodyId body) const;
  // Returns the body of instance `id`'s own prefab, or nullopt.
  [[nodiscard]] std::optional<physics::BodyId> BodyFor(
      scene::InstanceId id) const;

  // Every body of the last Build(), in creation order (removed ones
  // included, marked).
  [[nodiscard]] std::span<const CollisionBody> bodies() const {
    return bodies_;
  }
  // Appends the bodies (not removed) of the instances' own prefabs that ask
  // for dynamic motion: the scene objects a simulation moves, keyed by the
  // instance that owns them.
  void AppendDynamicRoots(std::vector<CollisionBody>& out) const;

  [[nodiscard]] physics::PhysicsWorld& physics() { return physics_; }
  [[nodiscard]] const physics::PhysicsWorld& physics() const {
    return physics_;
  }

 private:
  // Returns the shape of `prefab`'s collider, restored on first use, or
  // nullptr when it cannot be restored (logged).
  const physics::CookedShape* ShapeFor(PrefabId prefab,
                                       const PrefabSource& prefabs);

  physics::PhysicsWorld& physics_;
  CollisionWorldOptions options_;
  std::vector<CollisionBody> bodies_;
  std::unordered_map<uint32_t, size_t> by_body_;  // BodyId value -> entry
  // Shapes restored from prefabs, by PrefabId value; shared by the
  // placements of one Build() and cleared by the next.
  std::unordered_map<uint32_t, physics::CookedShape> shapes_;
};

}  // namespace blue::world

#endif  // BLUE_WORLD_CORE_COLLISION_WORLD_H_
