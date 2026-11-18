#ifndef BLUE_RENDER_FRAME_GLOBALS_H_
#define BLUE_RENDER_FRAME_GLOBALS_H_

#include <vulkan/vulkan.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include <glm/ext/vector_uint4.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include "lighting/cascades.h"
#include "render/buffer.h"
#include "render/frame_config.h"
#include "render/image.h"
#include "render/samplers.h"
#include "render/scene_view.h"
#include "render/vulkan_context.h"

namespace blue::render {

// One point light as the shaders read it. Must match GpuPointLight in
// shaders/common.glsl (std140).
struct GpuPointLight {
  // xyz: world position, w: 1 / range^2, or 0 for an unlimited range.
  glm::vec4 position_inverse_range_sq;
  glm::vec4 radiance;  // rgb: color * intensity, w: unused
};

// Must match FrameUniforms in shaders/common.glsl (std140; offsets are
// asserted in the .cc).
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
};

// Returns the uniforms of `view` for a render target of `extent`.
// `cascades` are only read when `shadows` is true. The environment fields
// stay zero (no environment yet). Point lights whose range sphere lies
// outside the camera frustum are dropped; when more than kMaxPointLights
// remain, the ones that light the camera position the least are dropped.
// Allocates nothing.
[[nodiscard]] FrameUniforms MakeFrameUniforms(
    const SceneView& view, std::span<const lighting::Cascade> cascades,
    bool shadows, VkExtent2D extent);

// Descriptor set 0 of the scene passes, one per frame in flight:
//   binding 0: FrameUniforms (uniform buffer)
//   binding 1: cascaded shadow map (sampler2DArrayShadow)
//   binding 2: prefiltered environment cube (samplerCube), specular IBL
//   binding 3: BRDF lookup table (sampler2D, RG: scale, bias)
//   binding 4: ambient occlusion (sampler2D, R, render-target sized)
// Bindings 2-4 start bound to 1 x 1 defaults (black cube, zero LUT, white
// AO) so shaders can sample them before the passes that produce them exist.
// Borrows `context` and `samplers`, which must outlive it. Neither copyable
// nor movable. Not thread-safe.
class FrameGlobals {
 public:
  FrameGlobals(const VulkanContext& context, const Samplers& samplers);
  ~FrameGlobals();

  FrameGlobals(const FrameGlobals&) = delete;
  FrameGlobals& operator=(const FrameGlobals&) = delete;

  [[nodiscard]] VkDescriptorSetLayout set_layout() const { return layout_; }
  [[nodiscard]] VkDescriptorSet set(size_t frame) const {
    return frames_[frame].set;
  }
  // Copies `uniforms` into the buffer of `frame`, whose previous use the GPU
  // must have finished (its fence waited on).
  void Write(size_t frame, const FrameUniforms& uniforms) const;

  // The Bind* calls rewrite a binding of every frame's set: no frame may be
  // in flight (call while the GPU is idle, e.g. at startup or after a
  // swapchain rebuild). Images must be in SHADER_READ_ONLY_OPTIMAL (the
  // shadow map: DEPTH_READ_ONLY_OPTIMAL) whenever a frame samples them.
  void BindShadowMap(VkImageView view, VkSampler compare_sampler);
  void BindEnvironment(VkImageView prefiltered_cube, VkImageView brdf_lut);
  void BindAmbientOcclusion(VkImageView ambient_occlusion);

 private:
  struct Frame {
    Buffer uniforms;
    VkDescriptorSet set = VK_NULL_HANDLE;
  };

  // Points `binding` of every frame's set at `view` with `sampler`.
  void BindImage(uint32_t binding, VkImageView view, VkSampler sampler,
                 VkImageLayout layout);

  VkDevice device_;
  const Samplers& samplers_;
  VkDescriptorSetLayout layout_ = VK_NULL_HANDLE;
  VkDescriptorPool pool_ = VK_NULL_HANDLE;
  std::array<Frame, kFramesInFlight> frames_;
  Image default_cube_;
  Image default_lut_;
  Image default_ambient_occlusion_;
};

}  // namespace blue::render

#endif  // BLUE_RENDER_FRAME_GLOBALS_H_
