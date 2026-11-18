#include "editor/viewport/debug_draw.h"

#include <array>
#include <cmath>
#include <numbers>

#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/vec4.hpp>

namespace blue::editor {
namespace {

using render::DebugLines;
using render::LineDepth;

constexpr float kPi = std::numbers::pi_v<float>;

// Returns `p` after `m`.
glm::vec3 Apply(const glm::mat4& m, const glm::vec3& p) {
  return glm::vec3(m * glm::vec4(p, 1.0f));
}

// Adds a circle of `radius` in the local plane spanned by `u` and `v` (unit,
// orthogonal) around `center`, all in the space of `m`; `from`..`to` is the
// arc in radians.
void AddArc(DebugLines& lines, const glm::mat4& m, const glm::vec3& center,
            const glm::vec3& u, const glm::vec3& v, float radius, float from,
            float to, const glm::vec4& color) {
  constexpr int kSegments = 24;
  glm::vec3 previous =
      Apply(m, center + (u * std::cos(from) + v * std::sin(from)) * radius);
  for (int i = 1; i <= kSegments; ++i) {
    const float angle = from + (to - from) * static_cast<float>(i) /
                                   static_cast<float>(kSegments);
    const glm::vec3 next =
        Apply(m, center + (u * std::cos(angle) + v * std::sin(angle)) * radius);
    lines.AddLine(previous, next, color);
    previous = next;
  }
}

// Adds a capsule along local Y: two hemispheres of `radius` at +-half_height
// joined by four lines, in the space of `m`.
void AddCapsule(DebugLines& lines, const glm::mat4& m, float radius,
                float half_height, const glm::vec4& color) {
  const glm::vec3 x(1.0f, 0.0f, 0.0f);
  const glm::vec3 y(0.0f, 1.0f, 0.0f);
  const glm::vec3 z(0.0f, 0.0f, 1.0f);
  const glm::vec3 top = y * half_height;
  const glm::vec3 bottom = -top;
  for (const glm::vec3& ring : {top, bottom}) {
    AddArc(lines, m, ring, x, z, radius, 0.0f, 2.0f * kPi, color);
  }
  // Caps: half circles in the XY and ZY planes.
  for (const glm::vec3& side : {x, z}) {
    AddArc(lines, m, top, side, y, radius, 0.0f, kPi, color);
    AddArc(lines, m, bottom, side, y, radius, kPi, 2.0f * kPi, color);
    lines.AddLine(Apply(m, top + side * radius),
                  Apply(m, bottom + side * radius), color);
    lines.AddLine(Apply(m, top - side * radius),
                  Apply(m, bottom - side * radius), color);
  }
}

}  // namespace

void AddGroundGrid(DebugLines& lines, const glm::vec3& focus, float half_extent,
                   float height) {
  lines.AddGrid({focus.x, height, focus.z}, half_extent, 1.0f, kGridColor, 10,
                kGridMajorColor);
}

void AddBounds(DebugLines& lines, const asset::Bounds& local_bounds,
               const glm::mat4& world, const glm::vec4& color) {
  lines.AddBox(local_bounds, world, color);
}

void AddCollider(DebugLines& lines, const prefab::Physics& physics,
                 const glm::mat4& prefab_world,
                 const asset::Bounds& fallback_bounds, const glm::vec4& color) {
  const glm::mat4 shape_world =
      prefab_world * glm::translate(glm::mat4(1.0f), physics.center) *
      glm::mat4_cast(physics.rotation);
  switch (physics.kind) {
    case prefab::ColliderKind::kNone:
      return;
    case prefab::ColliderKind::kBox:
      lines.AddBox({.min = -physics.half_extents, .max = physics.half_extents},
                   shape_world, color);
      return;
    case prefab::ColliderKind::kSphere:
      AddCapsule(lines, shape_world, physics.radius, 0.0f, color);
      return;
    case prefab::ColliderKind::kCapsule:
      AddCapsule(lines, shape_world, physics.radius, physics.half_height,
                 color);
      return;
    case prefab::ColliderKind::kConvexHull:
    case prefab::ColliderKind::kTriangleMesh:
      lines.AddBox(fallback_bounds, prefab_world, color);
      return;
  }
}

void AddSocket(DebugLines& lines, const glm::mat4& socket_world, float size) {
  // Normalise the axes so a scaled parent does not shrink the marker.
  glm::mat4 frame(1.0f);
  for (int axis = 0; axis < 3; ++axis) {
    const glm::vec3 direction(socket_world[axis]);
    const float length = glm::length(direction);
    frame[axis] = glm::vec4(
        length > 1.0e-8f ? direction / length : glm::vec3(0.0f), 0.0f);
  }
  frame[3] = socket_world[3];
  lines.AddAxes(frame, size, LineDepth::kAlwaysOnTop);
}

void AddPointLight(DebugLines& lines, const glm::vec3& position, float range,
                   bool show_range, const glm::vec4& color) {
  constexpr float kIconSize = 0.12f;
  lines.AddMarker(position, kIconSize, color, LineDepth::kAlwaysOnTop);
  // Diagonals make it read as a star rather than a socket cross.
  const float d = kIconSize * 0.7f;
  for (const glm::vec3& diagonal :
       {glm::vec3(d, d, 0.0f), glm::vec3(d, -d, 0.0f), glm::vec3(0.0f, d, d),
        glm::vec3(0.0f, d, -d)}) {
    lines.AddLine(position - diagonal, position + diagonal, color,
                  LineDepth::kAlwaysOnTop);
  }
  if (show_range && range > 0.0f) {
    lines.AddSphere(position, range, color * glm::vec4(1.0f, 1.0f, 1.0f, 0.5f));
  }
}

void AddSunArrow(DebugLines& lines, const glm::vec3& anchor,
                 const glm::vec3& direction, float length) {
  const float norm = glm::length(direction);
  if (!(norm > 1.0e-6f)) {
    return;
  }
  const glm::vec3 d = direction / norm;
  const glm::vec3 start = anchor - d * length;
  lines.AddLine(start, anchor, kLightColor, LineDepth::kAlwaysOnTop);
  // Arrow head: four short lines back from the tip.
  const glm::vec3 helper = std::abs(d.y) < 0.99f ? glm::vec3(0.0f, 1.0f, 0.0f)
                                                 : glm::vec3(1.0f, 0.0f, 0.0f);
  const glm::vec3 u = glm::normalize(glm::cross(helper, d));
  const glm::vec3 v = glm::cross(d, u);
  const float head = length * 0.15f;
  for (const glm::vec3& side : {u, -u, v, -v}) {
    lines.AddLine(anchor, anchor - d * head + side * head * 0.5f, kLightColor,
                  LineDepth::kAlwaysOnTop);
  }
  lines.AddCircle(start, d, length * 0.12f, kLightColor, 16,
                  LineDepth::kAlwaysOnTop);
}

void AddPlayerStart(DebugLines& lines, const glm::vec3& position,
                    float yaw_radians, const glm::vec4& color) {
  constexpr float kHeight = 1.8f;
  constexpr float kRadius = 0.3f;
  const glm::mat4 m = glm::translate(
      glm::mat4(1.0f), position + glm::vec3(0.0f, kHeight * 0.5f, 0.0f));
  AddCapsule(lines, m, kRadius, kHeight * 0.5f - kRadius, color);
  // View direction at eye height: yaw 0 looks down -Z, positive turns left.
  const glm::vec3 forward(-std::sin(yaw_radians), 0.0f, -std::cos(yaw_radians));
  const glm::vec3 eye = position + glm::vec3(0.0f, kHeight * 0.9f, 0.0f);
  const glm::vec3 tip = eye + forward * 0.8f;
  lines.AddLine(eye, tip, color, LineDepth::kAlwaysOnTop);
  const glm::vec3 side(-forward.z, 0.0f, forward.x);
  lines.AddLine(tip, tip - forward * 0.2f + side * 0.1f, color,
                LineDepth::kAlwaysOnTop);
  lines.AddLine(tip, tip - forward * 0.2f - side * 0.1f, color,
                LineDepth::kAlwaysOnTop);
}

}  // namespace blue::editor
