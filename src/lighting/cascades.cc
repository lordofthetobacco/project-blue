#include "lighting/cascades.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

namespace blue::lighting {
namespace {

// Radii are rounded up to a multiple of this many metres.
constexpr float kRadiusStep = 1.0f / 16.0f;

// Relative growth of the radius before rounding. Snapping the centre to the
// texel grid moves it by up to 0.71 texels (about r / 1450 metres), which
// would otherwise let corners of the slice poke out of the clip volume.
constexpr float kRadiusPadding = 1.0f / 1024.0f;

// Returns the up vector for a light looking along `direction`: world up,
// unless the light is nearly vertical, where world -Z is used instead.
glm::vec3 LightUp(const glm::vec3& direction) {
  return std::abs(direction.y) > 0.999f ? glm::vec3(0.0f, 0.0f, -1.0f)
                                        : glm::vec3(0.0f, 1.0f, 0.0f);
}

// Returns the world-space corners of the camera frustum between the view-space
// distances `split_near` and `split_far`.
std::array<glm::vec3, 8> SliceCorners(const glm::mat4& camera_view,
                                      float vertical_fov_radians, float aspect,
                                      float split_near, float split_far) {
  const glm::mat4 view_to_world = glm::inverse(camera_view);
  const float tan_half_fov = std::tan(vertical_fov_radians * 0.5f);
  std::array<glm::vec3, 8> corners;
  int index = 0;
  for (const float distance : {split_near, split_far}) {
    const float half_height = distance * tan_half_fov;
    const float half_width = half_height * aspect;
    for (const float sign_y : {-1.0f, 1.0f}) {
      for (const float sign_x : {-1.0f, 1.0f}) {
        const glm::vec4 view_corner(sign_x * half_width, sign_y * half_height,
                                    -distance, 1.0f);
        corners[index++] = glm::vec3(view_to_world * view_corner);
      }
    }
  }
  return corners;
}

}  // namespace

std::vector<float> ComputeCascadeSplits(float near_plane, float shadow_distance,
                                        int count, float lambda) {
  std::vector<float> splits(count);
  for (int i = 0; i < count; ++i) {
    const float fraction =
        static_cast<float>(i + 1) / static_cast<float>(count);
    const float uniform =
        near_plane + (shadow_distance - near_plane) * fraction;
    const float logarithmic =
        near_plane * std::pow(shadow_distance / near_plane, fraction);
    splits[i] = glm::mix(uniform, logarithmic, lambda);
  }
  // Exact, whatever the rounding of pow() above.
  splits.back() = shadow_distance;
  return splits;
}

// Fitting method:
//  1. The slice's eight corners are taken to world space.
//  2. A bounding sphere is built around them: centre = corner centroid,
//     radius = farthest corner, padded by kRadiusPadding and rounded up to
//     kRadiusStep. A sphere does not change size when the camera only rotates,
//     and the rounding keeps r constant against float noise, so the shadow map
//     keeps a constant world-space resolution.
//  3. The light looks along `sun_direction`; its orthographic volume covers
//     [-r, r] in x and y. In depth it reaches r behind the centre and
//     r + kCasterExtension towards the sun, so off-screen casters still cast.
//  4. The centre is snapped to whole texels (2r / shadow_map_size) in light
//     space, so moving the camera slides the shadow map in texel steps and
//     shadow edges do not shimmer.
//
// The main camera renders with reversed-Z and an infinite far plane, so the
// cascade distances are explicit view-space distances, not derived from the
// main projection matrix.
Cascade FitCascade(const glm::mat4& camera_view, float vertical_fov_radians,
                   float aspect, float split_near, float split_far,
                   glm::vec3 sun_direction, int shadow_map_size) {
  const std::array<glm::vec3, 8> corners = SliceCorners(
      camera_view, vertical_fov_radians, aspect, split_near, split_far);

  glm::vec3 center(0.0f);
  for (const glm::vec3& corner : corners) {
    center += corner;
  }
  center /= static_cast<float>(corners.size());
  float radius = 0.0f;
  for (const glm::vec3& corner : corners) {
    radius = std::max(radius, glm::distance(corner, center));
  }
  radius =
      std::ceil(radius * (1.0f + kRadiusPadding) / kRadiusStep) * kRadiusStep;
  const float texel_world_size =
      2.0f * radius / static_cast<float>(shadow_map_size);

  // Light rotation only (eye at the origin); the translation is added below.
  const glm::vec3 direction = glm::normalize(sun_direction);
  const glm::mat4 light_rotation =
      glm::lookAtRH(glm::vec3(0.0f), direction, LightUp(direction));
  glm::vec3 center_light = glm::vec3(light_rotation * glm::vec4(center, 1.0f));
  center_light.x =
      std::round(center_light.x / texel_world_size) * texel_world_size;
  center_light.y =
      std::round(center_light.y / texel_world_size) * texel_world_size;

  const glm::mat4 view =
      glm::translate(glm::mat4(1.0f), -center_light) * light_rotation;
  // View space looks down -Z, so a negative near distance reaches towards the
  // sun.
  const glm::mat4 projection = glm::orthoRH_ZO(
      -radius, radius, -radius, radius, -(radius + kCasterExtension), radius);

  return Cascade{.view_projection = projection * view,
                 .split_far = split_far,
                 .texel_world_size = texel_world_size};
}

std::array<Cascade, kCascadeCount> FitCascades(const glm::mat4& camera_view,
                                               float vertical_fov_radians,
                                               float aspect, float near_plane,
                                               glm::vec3 sun_direction) {
  const std::vector<float> splits = ComputeCascadeSplits(
      near_plane, kShadowDistance, kCascadeCount, kSplitLambda);
  std::array<Cascade, kCascadeCount> cascades;
  float split_near = near_plane;
  for (int i = 0; i < kCascadeCount; ++i) {
    cascades[i] =
        FitCascade(camera_view, vertical_fov_radians, aspect, split_near,
                   splits[i], sun_direction, kShadowMapSize);
    split_near = splits[i];
  }
  return cascades;
}

}  // namespace blue::lighting
