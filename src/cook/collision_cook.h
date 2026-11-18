#ifndef BLUE_COOK_COLLISION_COOK_H_
#define BLUE_COOK_COLLISION_COOK_H_

#include "cook/cook_error.h"
#include "core/result.h"
#include "prefab/prefab.h"

// Collision cooking for prefabs: fits or takes primitive shapes, builds
// convex hulls and triangle meshes from the prefab's own meshes, and stores
// Jolt's binary shape in prefab.physics.cooked_shape (see
// physics/shape_cooking.h, which restores it at load time). Needs a live
// physics::PhysicsRuntime.

namespace blue::cook {

// How CookCollision() sizes primitive shapes.
enum class ShapeFit {
  // Box, sphere and capsule are fitted around prefab.bounds (centre,
  // identity rotation; the capsule stands along Y).
  kBounds,
  // The centre, rotation and size fields of the given settings are used.
  kParameters,
};

// Sets prefab.physics to `settings` (body flags, mass, friction,
// restitution, kind) and cooks its shape:
//   kNone          clears the body (no cooked shape);
//   kBox/kSphere/kCapsule  sized by `fit`; the fitted parameters are written
//                  back into prefab.physics;
//   kConvexHull    hull of every mesh vertex at the rest pose, prefab space;
//   kTriangleMesh  every mesh triangle at the rest pose (static only).
// settings.cooked_shape is ignored. The previous cooked shape stays in
// storage until the prefab is saved and reloaded. Fails with
// kInvalidArgument for a dynamic triangle mesh, non-finite or negative
// parameters, or a mesh shape without meshes, and kShapeCookFailed when the
// physics library rejects the shape; the prefab is unchanged on failure.
[[nodiscard]] core::Result<void, CookError> CookCollision(
    prefab::Prefab& prefab, const prefab::Physics& settings,
    ShapeFit fit = ShapeFit::kBounds);

}  // namespace blue::cook

#endif  // BLUE_COOK_COLLISION_COOK_H_
