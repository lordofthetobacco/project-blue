#ifndef BLUE_RENDER_FRAME_UNIFORMS_H_
#define BLUE_RENDER_FRAME_UNIFORMS_H_

#include <array>
#include <span>

#include <glm/ext/vector_uint4.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

#include "core/extent.h"
#include "lighting/cascades.h"
#include "render/scene_view.h"

namespace blue::render {

// One point light as the shaders read it. Must match GpuPointLight in
// shaders/common.h (identical layout).
struct GpuPointLight {
  // xyz: world position, w: 1 / range^2, or 0 for an unlimited range.
  glm::vec4 position_inverse_range_sq;
  glm::vec4 radiance;  // rgb: color * intensity, w: unused
};

// Must match FrameUniforms in shaders/common.h (identical layout; offsets
// are asserted in the .cc).
struct FrameUniforms {
  glm::mat4 view_proj;
  glm::mat4 view;
  glm::mat4 projection;
  glm::mat4 inverse_projection;
  glm::mat4 inverse_view_proj;
  glm::vec4 camera_position;
  glm::vec4 ambient_color;
  glm::vec4 sun_direction;  // xyz: direction the light travels
  glm::vec4 sun_color;      // rgb: color * intensity
  std::array<glm::mat4, lighting::kCascadeCount> cascade_view_projection;
  glm::vec4 cascade_split_far;  // View-space far distance per cascade.
  glm::vec4 cascade_texel_world_size;
  // x: shadows enabled, y: cascade debug, z: shadow distance, w: normal
  // offset scale in texels.
  glm::vec4 shadow_params;
  // xy: render target size in pixels, zw: its reciprocal.
  glm::vec4 screen;
  // Diffuse irradiance of the environment as 9 spherical-harmonics
  // coefficients (rgb each, lighting::IrradianceSh9 with the constants
  // folded in). All zero while no environment is bound.
  std::array<glm::vec4, 9> irradiance_sh;
  // x: intensity, y: rotation about +Y (radians), z: highest mip of the
  // prefiltered cube, w: 1 when an environment is bound, else 0.
  glm::vec4 environment_params;
  // x: number of valid point_lights, [0, kMaxPointLights].
  glm::uvec4 point_light_count;
  std::array<GpuPointLight, kMaxPointLights> point_lights;
  // Camera::previous_view_projection (never jittered), for the motion
  // vectors of the weapon motion blur (render/motion_blur_math.h) and of the
  // scene (MotionVectorPass).
  glm::mat4 previous_view_proj;
  // x: blur scale (MotionBlurScale(); 0 when the blur is off), y: longest
  // blur vector in pixels (twice z), z: largest blur radius in pixels
  // (MotionBlurMaxRadius(), the tile size of RenderTargets::motion_tiles()),
  // w: unused.
  glm::vec4 motion_blur;
  // The camera's projection * view without the jitter: where a point is on
  // screen as far as motion goes. Equal to view_proj without jitter.
  glm::mat4 unjittered_view_proj;
  // xy: the jitter in pixels of the render target (x right, y down), the
  // `jitter_pixels` of MakeFrameUniforms(); zw: unused.
  glm::vec4 jitter;
};

// Returns the uniforms of `view` for a render target of `extent`.
// `cascades` are only read when `shadows` is true. The environment fields
// stay zero (no environment yet). The motion blur scale is 0 when
// view.post disables the weapon motion blur. Point lights whose range sphere
// lies outside the camera frustum are dropped; when more than kMaxPointLights
// remain, the ones that light the camera position the least are dropped.
//
// `jitter_pixels` (JitterOffset() of render/temporal_math.h; zero without
// the temporal scaler) moves what is rasterized by that many pixels of the
// target: projection, view_proj and their inverses carry it
// (JitterProjection()), so every pass that draws or reconstructs positions
// from the depth buffer agrees. unjittered_view_proj and previous_view_proj
// never carry it. With zero jitter every field is exactly what it was
// before jitter existed, and unjittered_view_proj equals view_proj.
// Allocates nothing.
[[nodiscard]] FrameUniforms MakeFrameUniforms(
    const SceneView& view, std::span<const lighting::Cascade> cascades,
    bool shadows, core::Extent2D extent, glm::vec2 jitter_pixels = {});

}  // namespace blue::render

#endif  // BLUE_RENDER_FRAME_UNIFORMS_H_
