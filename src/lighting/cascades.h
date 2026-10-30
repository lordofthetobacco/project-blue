#ifndef BLUE_LIGHTING_CASCADES_H_
#define BLUE_LIGHTING_CASCADES_H_

#include <array>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

// Cascaded shadow map math for a directional light. Pure CPU code on glm: no
// graphics API types, so it is unit-testable without a device.

namespace blue::lighting {

inline constexpr int kCascadeCount = 4;
inline constexpr int kShadowMapSize = 2048;  // Texels per side, per cascade.
inline constexpr float kShadowDistance = 120.0f;  // Metres.
inline constexpr float kSplitLambda = 0.75f;  // 0 = uniform, 1 = logarithmic.
// Metres of extra depth towards the sun, so casters outside the camera
// frustum still shadow it.
inline constexpr float kCasterExtension = 200.0f;

// One shadow cascade: an orthographic light-space fit of a camera frustum
// slice.
struct Cascade {
  // World to light clip space: x and y in [-1, 1], depth in [0, 1].
  glm::mat4 view_projection;
  float split_far;         // View-space far distance of this cascade, metres.
  float texel_world_size;  // World-space size of one shadow-map texel, metres.
};

// Returns the view-space far distance of each of `count` cascades covering
// [near_plane, shadow_distance] with the practical split scheme: a blend of
// uniform (`lambda` = 0) and logarithmic (`lambda` = 1) spacing. The values
// are strictly increasing and the last equals `shadow_distance`. Requires
// 0 < near_plane < shadow_distance, count >= 1 and lambda in [0, 1].
[[nodiscard]] std::vector<float> ComputeCascadeSplits(float near_plane,
                                                      float shadow_distance,
                                                      int count, float lambda);

// Fits one cascade around the slice [split_near, split_far] (view-space
// distances) of the camera frustum. `camera_view` is the world-to-view matrix
// (looking down -Z), `sun_direction` the direction the light travels (need
// not be normalized, must be non-zero) and `shadow_map_size` the texels per
// side. See cascades.cc for the fitting method.
[[nodiscard]] Cascade FitCascade(const glm::mat4& camera_view,
                                 float vertical_fov_radians, float aspect,
                                 float split_near, float split_far,
                                 glm::vec3 sun_direction, int shadow_map_size);

// Returns all cascades for one camera, using kCascadeCount, kShadowDistance,
// kSplitLambda and kShadowMapSize; cascade i covers the slice between split
// i - 1 (`near_plane` for the first) and split i.
[[nodiscard]] std::array<Cascade, kCascadeCount> FitCascades(
    const glm::mat4& camera_view, float vertical_fov_radians, float aspect,
    float near_plane, glm::vec3 sun_direction);

}  // namespace blue::lighting

#endif  // BLUE_LIGHTING_CASCADES_H_
