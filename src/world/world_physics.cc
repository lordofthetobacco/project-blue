#include "world/world_physics.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/mat3x3.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include "core/log.h"
#include "physics/physics_world.h"
#include "physics/shape_cooking.h"
#include "prefab/prefab.h"
#include "scene/scene.h"
#include "scene/scene_data.h"

namespace blue::world {
namespace {

using ShapeResult = core::Result<physics::CookedShape, physics::PhysicsError>;

// The cooked-shape description of a primitive prefab collider.
std::optional<physics::ShapeCookDesc> PrimitiveDesc(
    const prefab::Physics& physics) {
  physics::ShapeCookDesc desc{.center = physics.center,
                              .rotation = physics.rotation,
                              .half_extents = physics.half_extents,
                              .radius = physics.radius,
                              .half_height = physics.half_height};
  switch (physics.kind) {
    case prefab::ColliderKind::kBox:
      desc.kind = physics::CookedShapeKind::kBox;
      return desc;
    case prefab::ColliderKind::kSphere:
      desc.kind = physics::CookedShapeKind::kSphere;
      return desc;
    case prefab::ColliderKind::kCapsule:
      desc.kind = physics::CookedShapeKind::kCapsule;
      return desc;
    case prefab::ColliderKind::kNone:
    case prefab::ColliderKind::kConvexHull:
    case prefab::ColliderKind::kTriangleMesh:
      break;
  }
  return std::nullopt;
}

// Restores the collider of `physics`: its cooked bytes, or a primitive
// cooked from its parameters.
ShapeResult RestoreShape(const prefab::Physics& physics) {
  if (!physics.cooked_shape.empty()) {
    return physics::CookedShape::Restore(physics.cooked_shape);
  }
  const std::optional<physics::ShapeCookDesc> desc = PrimitiveDesc(physics);
  if (!desc) {
    return ShapeResult::Error(physics::PhysicsError::kInvalidShape);
  }
  auto bytes = physics::CookShape(*desc);
  if (!bytes.ok()) {
    return ShapeResult::Error(bytes.error());
  }
  return physics::CookedShape::Restore(bytes.value());
}

// A world matrix split into a body pose and the scale of its shape.
struct Decomposed {
  physics::Pose pose;
  glm::vec3 scale{1.0f};
};

// Splits `world` (translation * rotation * scale, no shear) into pose and
// scale.
Decomposed Decompose(const glm::mat4& world) {
  Decomposed result;
  result.pose.position = glm::vec3(world[3]);
  glm::mat3 rotation(world);
  for (int axis = 0; axis < 3; ++axis) {
    result.scale[axis] = glm::length(rotation[axis]);
    if (result.scale[axis] > 0.0f) {
      rotation[axis] /= result.scale[axis];
    }
  }
  result.pose.rotation = glm::normalize(glm::quat_cast(rotation));
  return result;
}

}  // namespace

PhysicsBuildSummary WorldPhysics::Build(const scene::SceneData& scene,
                                        const World& world) {
  for (const BodyEntry& entry : bodies_) {
    if (!entry.removed) {
      physics_.RemoveBody(entry.body);
    }
  }
  bodies_.clear();
  by_body_.clear();
  // Prefabs may have been replaced since the last build (editor).
  shapes_.clear();
  physics_.SetGravity(scene.gravity);

  PhysicsBuildSummary summary;
  const std::span<const Placement> placements = world.placements();
  for (size_t p = 0; p < placements.size(); ++p) {
    const Placement& placement = placements[p];
    const prefab::Physics& collider =
        world.cache().prefab(placement.prefab).physics;
    if (collider.kind == prefab::ColliderKind::kNone) {
      continue;
    }
    const scene::Instance* instance =
        scene::FindInstance(scene, placement.owner);
    const scene::PhysicsOverride overrides =
        instance != nullptr && instance->physics ? *instance->physics
                                                 : scene::PhysicsOverride{};
    if (overrides.disabled) {
      continue;
    }

    const auto key = static_cast<uint32_t>(placement.prefab);
    auto shape = shapes_.find(key);
    if (shape == shapes_.end()) {
      ShapeResult restored = RestoreShape(collider);
      if (!restored.ok()) {
        core::log::Warn("Collider of {}: {}",
                        world.cache().key(placement.prefab),
                        physics::ToString(restored.error()));
        ++summary.failed;
        continue;
      }
      shape = shapes_.emplace(key, std::move(restored).value()).first;
    }

    const Decomposed placed = Decompose(placement.world);
    const scene::MotionType motion = overrides.motion.value_or(
        collider.dynamic ? scene::MotionType::kDynamic
                         : scene::MotionType::kStatic);
    const physics::ShapeBodyDesc desc{
        .motion = motion,
        .pose = placed.pose,
        .scale = placed.scale,
        .mass = overrides.mass.value_or(collider.mass),
        .friction = overrides.friction.value_or(collider.friction),
        .restitution = overrides.restitution.value_or(collider.restitution)};
    auto body = physics_.CreateBody(shape->second, desc);
    if (!body.ok()) {
      core::log::Warn("Body for instance {} ({}): {}", placement.owner,
                      world.cache().key(placement.prefab),
                      physics::ToString(body.error()));
      ++summary.failed;
      continue;
    }
    by_body_.emplace(static_cast<uint32_t>(body.value()), bodies_.size());
    bodies_.push_back({.body = body.value(),
                       .instance = placement.owner,
                       .placement = p,
                       .scale = placed.scale,
                       .dynamic = motion == scene::MotionType::kDynamic,
                       .root = placement.parent < 0});
    ++summary.created;
  }
  return summary;
}

int WorldPhysics::RemoveHiddenBodies(const World& world) {
  int removed = 0;
  for (BodyEntry& entry : bodies_) {
    if (entry.removed || !world.IsInstanceHidden(entry.instance)) {
      continue;
    }
    physics_.RemoveBody(entry.body);
    by_body_.erase(static_cast<uint32_t>(entry.body));
    entry.removed = true;
    ++removed;
  }
  return removed;
}

void WorldPhysics::SyncToWorld(World& world) const {
  for (const BodyEntry& entry : bodies_) {
    if (!entry.dynamic || !entry.root || entry.removed) {
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
  auto found = by_body_.find(static_cast<uint32_t>(body));
  if (found == by_body_.end()) {
    return std::nullopt;
  }
  return bodies_[found->second].instance;
}

std::optional<physics::BodyId> WorldPhysics::BodyFor(
    scene::InstanceId id) const {
  for (const BodyEntry& entry : bodies_) {
    if (entry.instance == id && entry.root && !entry.removed) {
      return entry.body;
    }
  }
  return std::nullopt;
}

}  // namespace blue::world
