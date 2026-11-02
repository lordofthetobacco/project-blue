#ifndef BLUE_CAMERA_FRUSTUM_H_
#define BLUE_CAMERA_FRUSTUM_H_

#include <array>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

// View-frustum culling math. Pure glm, no graphics API types.

namespace blue::camera {

// Axis-aligned box in world space.
struct Aabb {
  glm::vec3 min{0.0f};
  glm::vec3 max{0.0f};

  [[nodiscard]] glm::vec3 center() const { return (min + max) * 0.5f; }
  [[nodiscard]] glm::vec3 half_extents() const { return (max - min) * 0.5f; }
};

// Returns the world-space box enclosing `local` after `transform` (rotation,
// scale and translation; the last row must be 0 0 0 1). The result is the
// tight box of the transformed box, so it can be larger than the transformed
// mesh itself.
[[nodiscard]] Aabb TransformAabb(const Aabb& local, const glm::mat4& transform);

// The visible volume of a clip-space matrix, as inward-facing planes. Works
// for perspective and orthographic projections with Vulkan clip conventions
// (x, y in [-w, w], z in [0, w]), including reversed-Z and infinite far
// planes, whose degenerate far plane is simply dropped. Copyable.
class Frustum {
 public:
  Frustum() = default;
  // `clip_from_world` is projection * view.
  explicit Frustum(const glm::mat4& clip_from_world);

  // Returns false only when `box` is certainly outside the frustum; a box
  // that is outside but near an edge may still return true.
  [[nodiscard]] bool Intersects(const Aabb& box) const;

  [[nodiscard]] int plane_count() const { return plane_count_; }

 private:
  // xyz: unit normal pointing inside; w: distance, so that
  // dot(normal, p) + w >= 0 for points inside.
  std::array<glm::vec4, 6> planes_{};
  int plane_count_ = 0;
};

}  // namespace blue::camera

#endif  // BLUE_CAMERA_FRUSTUM_H_
