#include "world/world_physics.h"

#include <memory>
#include <optional>
#include <span>

#include <glm/ext/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>

#include "physics/physics_world.h"
#include "scene/scene.h"
#include "scene/scene_data.h"
#include "world/core/collision_world.h"
#include "world/core/placement.h"

namespace blue::world {

WorldPhysics::WorldPhysics(physics::PhysicsWorld& physics)
    : physics_(physics),
      collision_(std::make_unique<CollisionWorld>(physics, options_)) {}

WorldPhysics::~WorldPhysics() = default;

PhysicsBuildSummary WorldPhysics::Build(const scene::SceneData& scene,
                                        const World& world,
                                        CollisionWorldOptions options) {
  if (options.dynamic_bodies != options_.dynamic_bodies) {
    // An empty build removes the old bodies first.
    (void)collision_->Build(scene, std::span<const Placement>(), world.cache());
    options_ = options;
    collision_ = std::make_unique<CollisionWorld>(physics_, options_);
  }
  return collision_->Build(scene, world.placements(), world.cache());
}

int WorldPhysics::RemoveHiddenBodies(const World& world) {
  int removed = 0;
  for (const CollisionBody& entry : collision_->bodies()) {
    if (!entry.removed && world.IsInstanceHidden(entry.instance)) {
      // Removes this entry and the later ones of the same instance.
      removed += collision_->RemoveInstanceBodies(entry.instance);
    }
  }
  return removed;
}

void WorldPhysics::SyncToWorld(World& world) const {
  const bool kinematic = options_.dynamic_bodies == DynamicBodies::kKinematic;
  for (const CollisionBody& entry : collision_->bodies()) {
    const bool moves =
        entry.simulated ||
        (kinematic && entry.motion == scene::MotionType::kDynamic);
    if (!moves || !entry.root || entry.removed) {
      continue;
    }
    const physics::Pose pose = physics_.InterpolatedPose(entry.body);
    const glm::mat4 matrix =
        glm::scale(glm::translate(glm::mat4(1.0f), pose.position) *
                       glm::mat4_cast(pose.rotation),
                   entry.scale);
    world.SetInstanceMatrix(entry.instance, matrix);
  }
}

std::optional<scene::InstanceId> WorldPhysics::InstanceFor(
    physics::BodyId body) const {
  return collision_->InstanceFor(body);
}

std::optional<physics::BodyId> WorldPhysics::BodyFor(
    scene::InstanceId id) const {
  return collision_->BodyFor(id);
}

}  // namespace blue::world
