#ifndef BLUE_RENDER_VULKAN_FRAME_GLOBALS_H_
#define BLUE_RENDER_VULKAN_FRAME_GLOBALS_H_

#include <vulkan/vulkan.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include <glm/ext/vector_uint4.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include "render/frame_config.h"
#include "render/frame_uniforms.h"
#include "render/scene_view.h"
#include "render/vulkan/buffer.h"
#include "render/vulkan/image.h"
#include "render/vulkan/samplers.h"
#include "render/vulkan/vulkan_context.h"

namespace blue::render::vulkan {

// The views a frame renders with their own uniforms: the camera's and a
// rifle scope's (SceneView::scope).
enum class FrameView : uint32_t {
  kMain = 0,
  kScope = 1,
};
inline constexpr size_t kFrameViewCount = 2;

// Descriptor set 0 of the scene passes, one per frame in flight and view:
//   binding 0: FrameUniforms (uniform buffer)
//   binding 1: cascaded shadow map (sampler2DArrayShadow)
//   binding 2: prefiltered environment cube (samplerCube), specular IBL
//   binding 3: BRDF lookup table (sampler2D, RG: scale, bias)
//   binding 4: ambient occlusion (sampler2D, R, render-target sized)
// Bindings 2-4 start bound to 1 x 1 defaults (black cube, zero LUT, white
// AO) so shaders can sample them before the passes that produce them exist.
// The scope view's sets share every image but the ambient occlusion, which
// stays the white default (no SSAO in the scope picture).
// Borrows `context` and `samplers`, which must outlive it. Neither copyable
// nor movable. Not thread-safe.
class FrameGlobals {
 public:
  FrameGlobals(const VulkanContext& context, const Samplers& samplers);
  ~FrameGlobals();

  FrameGlobals(const FrameGlobals&) = delete;
  FrameGlobals& operator=(const FrameGlobals&) = delete;

  [[nodiscard]] VkDescriptorSetLayout set_layout() const { return layout_; }
  [[nodiscard]] VkDescriptorSet set(size_t frame,
                                    FrameView view = FrameView::kMain) const {
    return frames_[Index(frame, view)].set;
  }
  // Copies `uniforms` into the buffer of `frame` and `view`, whose previous
  // use the GPU must have finished (its fence waited on).
  void Write(size_t frame, const FrameUniforms& uniforms,
             FrameView view = FrameView::kMain) const;

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

  static size_t Index(size_t frame, FrameView view) {
    return frame * kFrameViewCount + static_cast<size_t>(view);
  }
  // Points texture `binding` of every frame's set (only those of `only`
  // when set) at `view`.
  void BindImage(uint32_t binding, VkImageView view, VkImageLayout layout,
                 std::optional<FrameView> only = std::nullopt);
  // Points sampler `binding` of every frame's set at `sampler`.
  void BindSampler(uint32_t binding, VkSampler sampler);

  VkDevice device_;
  const Samplers& samplers_;
  VkDescriptorSetLayout layout_ = VK_NULL_HANDLE;
  VkDescriptorPool pool_ = VK_NULL_HANDLE;
  // Indexed by Index().
  std::array<Frame, kFramesInFlight * kFrameViewCount> frames_;
  Image default_cube_;
  Image default_lut_;
  Image default_ambient_occlusion_;
};

}  // namespace blue::render::vulkan

#endif  // BLUE_RENDER_VULKAN_FRAME_GLOBALS_H_
