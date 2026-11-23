#ifndef BLUE_RENDER_VULKAN_SAMPLERS_H_
#define BLUE_RENDER_VULKAN_SAMPLERS_H_

#include <vulkan/vulkan.h>

#include "render/vulkan/vulkan_context.h"

namespace blue::render::vulkan {

// The samplers shared by the whole renderer. Borrows the device of
// `context`, which must outlive this object. Neither copyable nor movable.
// Thread-safe to read.
class Samplers {
 public:
  // `max_anisotropy` caps the material sampler's anisotropy (further capped
  // by the device); 1 or less disables anisotropic filtering.
  Samplers(const VulkanContext& context, float max_anisotropy);
  ~Samplers();

  Samplers(const Samplers&) = delete;
  Samplers& operator=(const Samplers&) = delete;

  // Material textures: trilinear, repeating, anisotropic when the device
  // supports it and the constructor allowed it.
  [[nodiscard]] VkSampler material() const { return material_; }
  // Full-screen inputs and render targets: bilinear (and linear between
  // mips), clamped to the edge.
  [[nodiscard]] VkSampler linear_clamp() const { return linear_clamp_; }
  // Exact texel fetches by UV, clamped to the edge.
  [[nodiscard]] VkSampler nearest_clamp() const { return nearest_clamp_; }

 private:
  VkDevice device_;
  VkSampler material_ = VK_NULL_HANDLE;
  VkSampler linear_clamp_ = VK_NULL_HANDLE;
  VkSampler nearest_clamp_ = VK_NULL_HANDLE;
};

}  // namespace blue::render::vulkan

#endif  // BLUE_RENDER_VULKAN_SAMPLERS_H_
