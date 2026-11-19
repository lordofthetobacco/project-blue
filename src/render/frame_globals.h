#ifndef BLUE_RENDER_FRAME_GLOBALS_H_
#define BLUE_RENDER_FRAME_GLOBALS_H_

#include <vulkan/vulkan.h>

#include <array>
#include <cstddef>
#include <cstdint>

#include "render/buffer.h"
#include "render/frame_config.h"
#include "render/frame_uniforms.h"
#include "render/image.h"
#include "render/samplers.h"
#include "render/vulkan_context.h"

namespace blue::render {

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
