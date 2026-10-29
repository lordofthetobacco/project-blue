#ifndef BLUE_PHYSICS_INTERNAL_SHAPE_BUILDER_H_
#define BLUE_PHYSICS_INTERNAL_SHAPE_BUILDER_H_

// Internal to the physics library: this header exposes Jolt types, so it lives
// in internal/ and must not be included from the public physics headers.

// Jolt.h must be the first Jolt header included.
// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>
// clang-format on

#include "core/result.h"
#include "physics/physics_world.h"

namespace blue::physics::internal {

// Axes of a box thinner than this are thickened to it, see kBox in
// BuildBaseShape().
inline constexpr float kMinBoxThickness = 0.1f;

// Builds the unscaled collision shape for `desc`: a box or sphere fitted to
// desc.bounds, or a convex hull / triangle mesh of desc.mesh. Fails with
// kInvalidShape for bad bounds or missing/malformed mesh data,
// kDynamicTriangleMesh for a dynamic triangle mesh and kShapeBuildFailed when
// Jolt rejects the shape. Ignores desc.scale.
core::Result<JPH::ShapeRefC, PhysicsError> BuildBaseShape(const BodyDesc& desc);

// Returns `base` scaled by `scale` (components must be finite and non-zero),
// adjusted to the nearest scale the shape supports, or `base` itself for unit
// scale. Fails with kShapeBuildFailed if Jolt rejects the scaled shape.
core::Result<JPH::ShapeRefC, PhysicsError> ApplyScale(
    const JPH::ShapeRefC& base, glm::vec3 scale);

}  // namespace blue::physics::internal

#endif  // BLUE_PHYSICS_INTERNAL_SHAPE_BUILDER_H_
