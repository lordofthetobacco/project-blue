#include "camera/frustum.h"

#include <cmath>

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/mat3x3.hpp>

namespace blue::camera {
namespace {

// A plane whose normal is shorter than this came from a degenerate clip row
// (an infinite far plane) and constrains nothing.
constexpr float kMinNormalLength = 1.0e-6f;

// Returns row `index` of `m` as a vec4 (glm stores columns).
glm::vec4 Row(const glm::mat4& m, int index) {
  return {m[0][index], m[1][index], m[2][index], m[3][index]};
}

}  // namespace

Aabb TransformAabb(const Aabb& local, const glm::mat4& transform) {
  const glm::vec3 center =
      glm::vec3(transform * glm::vec4(local.center(), 1.0f));
  // Each world axis extent is the sum of the absolute contributions of the
  // local axes (Arvo's method).
  const glm::mat3 linear(transform);
  glm::vec3 half(0.0f);
  const glm::vec3 local_half = local.half_extents();
  for (int axis = 0; axis < 3; ++axis) {
    half += glm::abs(linear[axis]) * local_half[axis];
  }
  return {.min = center - half, .max = center + half};
}

Frustum::Frustum(const glm::mat4& clip_from_world) {
  // Gribb/Hartmann: a point is inside when each clip inequality holds, and
  // each inequality is a row combination of the matrix.
  const glm::vec4 x = Row(clip_from_world, 0);
  const glm::vec4 y = Row(clip_from_world, 1);
  const glm::vec4 z = Row(clip_from_world, 2);
  const glm::vec4 w = Row(clip_from_world, 3);
  const std::array<glm::vec4, 6> candidates{
      w + x,  // -w <= x  (left)
      w - x,  //  x <= w  (right)
      w + y,  // -w <= y  (bottom)
      w - y,  //  y <= w  (top)
      z,      //  0 <= z  (near, or far when the depth is reversed)
      w - z,  //  z <= w  (far, or near when the depth is reversed)
  };
  for (const glm::vec4& plane : candidates) {
    const float length = glm::length(glm::vec3(plane));
    if (length < kMinNormalLength) {
      continue;
    }
    planes_[static_cast<size_t>(plane_count_)] = plane / length;
    ++plane_count_;
  }
}

bool Frustum::Intersects(const Aabb& box) const {
  const glm::vec3 center = box.center();
  const glm::vec3 half = box.half_extents();
  for (int i = 0; i < plane_count_; ++i) {
    const glm::vec4& plane = planes_[static_cast<size_t>(i)];
    const glm::vec3 normal(plane);
    // Distance of the box's most inward corner; negative means the whole box
    // is behind this plane.
    const float radius = glm::dot(glm::abs(normal), half);
    if (glm::dot(normal, center) + plane.w + radius < 0.0f) {
      return false;
    }
  }
  return true;
}

}  // namespace blue::camera
