#ifndef BLUE_PHYSICS_SHAPE_COOKING_H_
#define BLUE_PHYSICS_SHAPE_COOKING_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include "asset/mesh_data.h"
#include "core/result.h"
#include "physics/physics_world.h"

// Collision shapes cooked offline into Jolt's binary shape state (stored in
// prefabs) and restored at load time without rebuilding hulls or mesh trees.
// Jolt stays private: no Jolt type appears here.
//
// Cooked bytes start with a small header (magic + Jolt version); Jolt's
// binary state is not portable across Jolt versions, so bytes cooked by
// another version fail to restore with kShapeBuildFailed and must be
// re-cooked from the source (the prefab keeps the shape parameters).
//
// Every function needs a live PhysicsRuntime.

namespace blue::physics {

enum class CookedShapeKind {
  kBox,           // half_extents
  kSphere,        // radius
  kCapsule,       // radius + half_height (cylinder part), along local Y
  kConvexHull,    // points
  kTriangleMesh,  // points + triangle_indices; static bodies only
};

// Input of CookShape(). Primitives are placed at `center` / `rotation`;
// hulls and meshes use `points` as given. Spans are borrowed for the call.
struct ShapeCookDesc {
  CookedShapeKind kind = CookedShapeKind::kBox;
  glm::vec3 center{0.0f};
  glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
  glm::vec3 half_extents{0.5f};
  float radius = 0.5f;
  float half_height = 0.5f;
  std::span<const glm::vec3> points;
  std::span<const uint32_t> triangle_indices;  // three per triangle
};

// Builds the shape `desc` describes and returns its cooked bytes. Fails with
// kInvalidShape for non-finite or non-positive sizes, too few points or bad
// triangle indices, and kShapeBuildFailed when Jolt rejects the shape.
[[nodiscard]] core::Result<std::vector<std::byte>, PhysicsError> CookShape(
    const ShapeCookDesc& desc);

struct ShapeRayHit {
  float distance = 0.0f;  // along the normalised ray direction
  glm::vec3 point{0.0f};
  glm::vec3 normal{0.0f};  // unit surface normal at `point`
};

namespace internal {
struct CookedShapeAccess;
}  // namespace internal

// A shape restored from cooked bytes. Copies share the same immutable Jolt
// shape (reference counted), so copying is cheap and copies may be used from
// several threads. Must be destroyed before the PhysicsRuntime.
class CookedShape {
 public:
  // Restores the shape from `bytes` (as produced by CookShape()). Fails with
  // kShapeBuildFailed for bytes that are truncated, corrupt or cooked by
  // another Jolt version.
  [[nodiscard]] static core::Result<CookedShape, PhysicsError> Restore(
      std::span<const std::byte> bytes);

  CookedShape(const CookedShape&) = default;
  CookedShape& operator=(const CookedShape&) = default;
  CookedShape(CookedShape&&) = default;
  CookedShape& operator=(CookedShape&&) = default;
  ~CookedShape();

  // Casts a ray from `origin` along `direction` (need not be normalised) up
  // to `max_distance` against the shape in its own space and returns the
  // closest hit, or nullopt. Rays starting inside a convex shape hit at
  // distance 0.
  [[nodiscard]] std::optional<ShapeRayHit> CastRay(glm::vec3 origin,
                                                   glm::vec3 direction,
                                                   float max_distance) const;

  // Returns the shape's local bounding box.
  [[nodiscard]] asset::Bounds bounds() const;

 private:
  friend struct internal::CookedShapeAccess;
  struct Holder;

  explicit CookedShape(std::shared_ptr<const Holder> holder);

  std::shared_ptr<const Holder> holder_;
};

}  // namespace blue::physics

#endif  // BLUE_PHYSICS_SHAPE_COOKING_H_
