#include "cook/collision_cook.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include <glm/ext/quaternion_float.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "animation/hierarchy.h"
#include "animation/transform.h"
#include "core/log.h"
#include "physics/shape_cooking.h"

namespace blue::cook {
namespace {

using Result = core::Result<void, CookError>;

// Boxes fitted to flat bounds get at least this half thickness, grown below
// the top face so the surface stays where it was (as physics::PhysicsWorld
// does for scene bodies).
constexpr float kMinHalfExtent = 0.05f;

// Jolt needs a cylinder part; a capsule fitted to a cube gets this much.
constexpr float kMinCapsuleHalfHeight = 1.0e-4f;

bool IsFinite(glm::vec3 v) {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

bool ValidSettings(const prefab::Physics& settings) {
  return IsFinite(settings.center) && std::isfinite(settings.rotation.w) &&
         std::isfinite(settings.rotation.x) &&
         std::isfinite(settings.rotation.y) &&
         std::isfinite(settings.rotation.z) &&
         IsFinite(settings.half_extents) && std::isfinite(settings.radius) &&
         std::isfinite(settings.half_height) && std::isfinite(settings.mass) &&
         settings.mass > 0.0f && std::isfinite(settings.friction) &&
         settings.friction >= 0.0f && std::isfinite(settings.restitution) &&
         settings.restitution >= 0.0f;
}

// Writes the primitive parameters fitted around `bounds` into `physics`.
void FitToBounds(const asset::Bounds& bounds, prefab::Physics& physics) {
  glm::vec3 half = bounds.size() * 0.5f;
  glm::vec3 center = bounds.center();
  physics.rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
  switch (physics.kind) {
    case prefab::ColliderKind::kBox:
      for (int axis = 0; axis < 3; ++axis) {
        if (half[axis] < kMinHalfExtent) {
          half[axis] = kMinHalfExtent;
          center[axis] = bounds.max[axis] - kMinHalfExtent;
        }
      }
      physics.half_extents = half;
      break;
    case prefab::ColliderKind::kSphere:
      physics.radius = std::max({half.x, half.y, half.z, kMinHalfExtent});
      break;
    case prefab::ColliderKind::kCapsule:
      // Upright and inside the bounds: as wide as the bounds allow without
      // growing past the top and bottom.
      physics.radius =
          std::max(std::min(std::max(half.x, half.z), half.y), kMinHalfExtent);
      physics.half_height =
          std::max(half.y - physics.radius, kMinCapsuleHalfHeight);
      break;
    case prefab::ColliderKind::kNone:
    case prefab::ColliderKind::kConvexHull:
    case prefab::ColliderKind::kTriangleMesh:
      break;
  }
  physics.center = center;
}

// Collects every mesh of `prefab` at the rest pose in prefab space.
void GatherGeometry(const prefab::Prefab& prefab,
                    std::vector<glm::vec3>& points,
                    std::vector<uint32_t>& indices) {
  std::vector<animation::Transform> pose(prefab.hierarchy.nodes.size());
  animation::ResetToRest(prefab.hierarchy, pose);
  std::vector<glm::mat4> world(pose.size());
  animation::ComputeWorldMatrices(prefab.hierarchy, pose, world);
  for (const prefab::Mesh& mesh : prefab.meshes) {
    const glm::mat4 matrix =
        mesh.node >= 0 && static_cast<size_t>(mesh.node) < world.size()
            ? world[static_cast<size_t>(mesh.node)]
            : glm::mat4(1.0f);
    const auto base = static_cast<uint32_t>(points.size());
    for (const asset::Vertex& vertex : mesh.vertices) {
      points.push_back(glm::vec3(matrix * glm::vec4(vertex.position, 1.0f)));
    }
    for (const asset::Index index : mesh.indices) {
      indices.push_back(base + index);
    }
  }
}

physics::CookedShapeKind ShapeKind(prefab::ColliderKind kind) {
  switch (kind) {
    case prefab::ColliderKind::kBox:
      return physics::CookedShapeKind::kBox;
    case prefab::ColliderKind::kSphere:
      return physics::CookedShapeKind::kSphere;
    case prefab::ColliderKind::kCapsule:
      return physics::CookedShapeKind::kCapsule;
    case prefab::ColliderKind::kConvexHull:
      return physics::CookedShapeKind::kConvexHull;
    case prefab::ColliderKind::kNone:
    case prefab::ColliderKind::kTriangleMesh:
      break;
  }
  return physics::CookedShapeKind::kTriangleMesh;
}

}  // namespace

core::Result<void, CookError> CookCollision(prefab::Prefab& prefab,
                                            const prefab::Physics& settings,
                                            ShapeFit fit) {
  prefab::Physics physics = settings;
  physics.cooked_shape = {};
  if (physics.kind == prefab::ColliderKind::kNone) {
    prefab.physics = physics;
    return Result::Ok();
  }
  if (!ValidSettings(physics) ||
      (physics.kind == prefab::ColliderKind::kTriangleMesh &&
       physics.dynamic)) {
    return Result::Error(CookError::kInvalidArgument);
  }
  const bool primitive = physics.kind == prefab::ColliderKind::kBox ||
                         physics.kind == prefab::ColliderKind::kSphere ||
                         physics.kind == prefab::ColliderKind::kCapsule;
  if (primitive && fit == ShapeFit::kBounds) {
    if (prefab.meshes.empty() || !IsFinite(prefab.bounds.min) ||
        !IsFinite(prefab.bounds.max)) {
      return Result::Error(CookError::kInvalidArgument);
    }
    FitToBounds(prefab.bounds, physics);
  }

  std::vector<glm::vec3> points;
  std::vector<uint32_t> indices;
  if (!primitive) {
    GatherGeometry(prefab, points, indices);
    if (points.empty()) {
      return Result::Error(CookError::kInvalidArgument);
    }
  }
  auto cooked = physics::CookShape({.kind = ShapeKind(physics.kind),
                                    .center = physics.center,
                                    .rotation = physics.rotation,
                                    .half_extents = physics.half_extents,
                                    .radius = physics.radius,
                                    .half_height = physics.half_height,
                                    .points = points,
                                    .triangle_indices = indices});
  if (!cooked.ok()) {
    core::log::Warn("CookCollision: {}", physics::ToString(cooked.error()));
    return Result::Error(cooked.error() == physics::PhysicsError::kInvalidShape
                             ? CookError::kInvalidArgument
                             : CookError::kShapeCookFailed);
  }
  physics.cooked_shape = prefab.storage.Adopt(std::move(cooked).value());
  prefab.physics = physics;
  return Result::Ok();
}

}  // namespace blue::cook
