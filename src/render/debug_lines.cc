#include "render/debug_lines.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

#include <glm/common.hpp>
#include <glm/geometric.hpp>

namespace blue::render {

uint32_t PackLineColor(const glm::vec4& color) {
  const glm::vec4 clamped = glm::clamp(color, glm::vec4(0.0f), glm::vec4(1.0f));
  uint32_t packed = 0;
  for (int i = 0; i < 4; ++i) {
    const auto channel =
        static_cast<uint32_t>(std::lround(clamped[i] * 255.0f));
    packed |= channel << (8 * i);
  }
  return packed;
}

void DebugLines::Clear() {
  tested_.clear();
  on_top_.clear();
}

void DebugLines::AddLine(const glm::vec3& a, const glm::vec3& b,
                         const glm::vec4& color, LineDepth depth) {
  const uint32_t packed = PackLineColor(color);
  std::vector<LineVertex>& list = List(depth);
  list.push_back({a, packed});
  list.push_back({b, packed});
}

void DebugLines::AddBox(const asset::Bounds& bounds, const glm::mat4& transform,
                        const glm::vec4& color, LineDepth depth) {
  // Corner i has x from bit 0, y from bit 1 and z from bit 2 (0 = min).
  std::array<glm::vec3, 8> corners;
  for (int i = 0; i < 8; ++i) {
    const glm::vec3 local((i & 1) != 0 ? bounds.max.x : bounds.min.x,
                          (i & 2) != 0 ? bounds.max.y : bounds.min.y,
                          (i & 4) != 0 ? bounds.max.z : bounds.min.z);
    corners[static_cast<size_t>(i)] =
        glm::vec3(transform * glm::vec4(local, 1.0f));
  }
  // Every pair of corners differing in exactly one bit is an edge.
  for (int i = 0; i < 8; ++i) {
    for (const int bit : {1, 2, 4}) {
      if ((i & bit) == 0) {
        AddLine(corners[static_cast<size_t>(i)],
                corners[static_cast<size_t>(i | bit)], color, depth);
      }
    }
  }
}

void DebugLines::AddAxes(const glm::mat4& transform, float length,
                         LineDepth depth) {
  const glm::vec3 origin(transform[3]);
  constexpr std::array<glm::vec4, 3> kColors = {
      glm::vec4(1.0f, 0.2f, 0.2f, 1.0f), glm::vec4(0.2f, 1.0f, 0.2f, 1.0f),
      glm::vec4(0.3f, 0.5f, 1.0f, 1.0f)};
  for (int axis = 0; axis < 3; ++axis) {
    const glm::vec3 direction(transform[axis]);
    AddLine(origin, origin + direction * length,
            kColors[static_cast<size_t>(axis)], depth);
  }
}

void DebugLines::AddCircle(const glm::vec3& center, const glm::vec3& normal,
                           float radius, const glm::vec4& color, int segments,
                           LineDepth depth) {
  const glm::vec3 n = glm::normalize(normal);
  // Any unit vector orthogonal to n, then the third axis of the frame.
  const glm::vec3 helper = std::abs(n.y) < 0.99f ? glm::vec3(0.0f, 1.0f, 0.0f)
                                                 : glm::vec3(1.0f, 0.0f, 0.0f);
  const glm::vec3 u = glm::normalize(glm::cross(helper, n)) * radius;
  const glm::vec3 v = glm::cross(n, u);
  const int count = std::max(segments, 3);
  const float step =
      2.0f * std::numbers::pi_v<float> / static_cast<float>(count);
  glm::vec3 previous = center + u;
  for (int i = 1; i <= count; ++i) {
    const float angle = step * static_cast<float>(i);
    const glm::vec3 next = center + u * std::cos(angle) + v * std::sin(angle);
    AddLine(previous, next, color, depth);
    previous = next;
  }
}

void DebugLines::AddSphere(const glm::vec3& center, float radius,
                           const glm::vec4& color, LineDepth depth) {
  AddCircle(center, glm::vec3(1.0f, 0.0f, 0.0f), radius, color, 32, depth);
  AddCircle(center, glm::vec3(0.0f, 1.0f, 0.0f), radius, color, 32, depth);
  AddCircle(center, glm::vec3(0.0f, 0.0f, 1.0f), radius, color, 32, depth);
}

void DebugLines::AddMarker(const glm::vec3& center, float size,
                           const glm::vec4& color, LineDepth depth) {
  for (int axis = 0; axis < 3; ++axis) {
    glm::vec3 offset(0.0f);
    offset[axis] = size;
    AddLine(center - offset, center + offset, color, depth);
  }
}

void DebugLines::AddGrid(const glm::vec3& center, float half_extent,
                         float spacing, const glm::vec4& color, int major_every,
                         const glm::vec4& major_color) {
  if (!(spacing > 0.0f) || !(half_extent > 0.0f)) {
    return;
  }
  const int half_count = static_cast<int>(std::floor(half_extent / spacing));
  const auto first_x = static_cast<int>(std::lround(center.x / spacing));
  const auto first_z = static_cast<int>(std::lround(center.z / spacing));
  const float x0 = static_cast<float>(first_x) * spacing;
  const float z0 = static_cast<float>(first_z) * spacing;
  const float extent = static_cast<float>(half_count) * spacing;
  const auto line_color = [&](int index) {
    return major_every > 0 && index % major_every == 0 ? major_color : color;
  };
  for (int i = -half_count; i <= half_count; ++i) {
    const float offset = static_cast<float>(i) * spacing;
    // Lines of constant x, then of constant z.
    AddLine({x0 + offset, center.y, z0 - extent},
            {x0 + offset, center.y, z0 + extent}, line_color(first_x + i));
    AddLine({x0 - extent, center.y, z0 + offset},
            {x0 + extent, center.y, z0 + offset}, line_color(first_z + i));
  }
}

void DebugLines::AddWireframe(const asset::MeshData& mesh,
                              const glm::mat4& transform,
                              const glm::vec4& color, LineDepth depth) {
  const uint32_t packed = PackLineColor(color);
  std::vector<LineVertex>& list = List(depth);
  const size_t triangle_end = mesh.indices.size() - mesh.indices.size() % 3;
  for (size_t i = 0; i < triangle_end; i += 3) {
    std::array<glm::vec3, 3> corners;
    for (size_t k = 0; k < 3; ++k) {
      const asset::Index index = mesh.indices[i + k];
      if (index >= mesh.vertices.size()) {
        return;  // Invalid mesh: draw what was valid so far.
      }
      corners[k] =
          glm::vec3(transform * glm::vec4(mesh.vertices[index].position, 1.0f));
    }
    for (size_t k = 0; k < 3; ++k) {
      list.push_back({corners[k], packed});
      list.push_back({corners[(k + 1) % 3], packed});
    }
  }
}

}  // namespace blue::render
