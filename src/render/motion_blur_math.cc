#include "render/motion_blur_math.h"

#include <array>
#include <cmath>
#include <limits>
#include <optional>

#include <glm/common.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

namespace blue::render {

namespace {

// A convex polygon in clip space. A quad clipped by kClipPlaneCount planes
// gains at most one vertex per plane.
struct ClipPolygon {
  std::array<glm::vec4, 8> vertices{};
  int count = 0;
};

constexpr int kClipPlaneCount = 3;

// Returns the signed distance of `clip` to clip plane `plane`, positive on
// the side the rasterizer keeps: z >= 0, z <= w, w >= kMotionBlurMinClipW.
// Each is affine in clip space, hence along every box edge.
float PlaneDistance(int plane, const glm::vec4& clip) {
  switch (plane) {
    case 0:
      return clip.z;
    case 1:
      return clip.w - clip.z;
    default:
      return clip.w - kMotionBlurMinClipW;
  }
}

// Returns the part of `polygon` on the kept side of `plane`
// (Sutherland-Hodgman).
ClipPolygon ClipAgainst(const ClipPolygon& polygon, int plane) {
  ClipPolygon kept;
  for (int i = 0; i < polygon.count; ++i) {
    const glm::vec4& a = polygon.vertices[i];
    const glm::vec4& b = polygon.vertices[(i + 1) % polygon.count];
    const float distance_a = PlaneDistance(plane, a);
    const float distance_b = PlaneDistance(plane, b);
    if (distance_a >= 0.0f) {
      kept.vertices[kept.count++] = a;
    }
    if ((distance_a >= 0.0f) != (distance_b >= 0.0f)) {
      const float t = distance_a / (distance_a - distance_b);
      kept.vertices[kept.count++] = a + (b - a) * t;
    }
  }
  return kept;
}

}  // namespace

std::optional<ScreenBounds> ProjectBoxToScreen(const glm::vec3& box_min,
                                               const glm::vec3& box_max,
                                               const glm::mat4& view_proj,
                                               const glm::vec2& extent) {
  // Corner i has x from bit 0, y from bit 1, z from bit 2 (0: min, 1: max).
  std::array<glm::vec4, 8> corners{};
  for (int i = 0; i < 8; ++i) {
    const glm::vec3 point((i & 1) != 0 ? box_max.x : box_min.x,
                          (i & 2) != 0 ? box_max.y : box_min.y,
                          (i & 4) != 0 ? box_max.z : box_min.z);
    corners[i] = view_proj * glm::vec4(point, 1.0f);
  }
  // The six faces, each a cycle of corners. The clipped box is the convex
  // hull of its clipped faces' vertices.
  constexpr std::array<std::array<int, 4>, 6> kFaces{{{0, 2, 6, 4},
                                                      {1, 3, 7, 5},
                                                      {0, 1, 5, 4},
                                                      {2, 3, 7, 6},
                                                      {0, 1, 3, 2},
                                                      {4, 5, 7, 6}}};
  ScreenBounds bounds{
      .min = glm::vec2(std::numeric_limits<float>::max()),
      .max = glm::vec2(std::numeric_limits<float>::lowest()),
  };
  bool any = false;
  bool finite = true;
  for (const std::array<int, 4>& face : kFaces) {
    ClipPolygon polygon;
    for (const int corner : face) {
      polygon.vertices[polygon.count++] = corners[corner];
    }
    for (int plane = 0; plane < kClipPlaneCount && polygon.count > 0; ++plane) {
      polygon = ClipAgainst(polygon, plane);
    }
    for (int i = 0; i < polygon.count; ++i) {
      const glm::vec4& clip = polygon.vertices[i];
      const glm::vec2 ndc = glm::vec2(clip) / clip.w;
      const glm::vec2 pixel((ndc.x * 0.5f + 0.5f) * extent.x,
                            (0.5f - ndc.y * 0.5f) * extent.y);
      // min / max would drop a NaN silently: keep it visible to the caller.
      finite = finite && std::isfinite(pixel.x) && std::isfinite(pixel.y);
      bounds.min = glm::min(bounds.min, pixel);
      bounds.max = glm::max(bounds.max, pixel);
      any = true;
    }
  }
  if (!any) {
    return std::nullopt;
  }
  if (!finite) {
    constexpr float kInfinity = std::numeric_limits<float>::infinity();
    return ScreenBounds{.min = glm::vec2(-kInfinity),
                        .max = glm::vec2(kInfinity)};
  }
  return bounds;
}

}  // namespace blue::render
