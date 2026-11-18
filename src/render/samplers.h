#ifndef BLUE_RENDER_SAMPLERS_H_
#define BLUE_RENDER_SAMPLERS_H_

#include <vulkan/vulkan.h>

#include "render/vulkan_context.h"

namespace blue::render {

// Most anisotropy any material sampler uses; capped by the device limit.
inline constexpr float kMaterialAnisotropy = 8.0f;

// The samplers shared by the whole renderer. Borrows the device of
// `context`, which must outlive this object. Neither copyable nor movable.
// Thread-safe to read.
class Samplers {
 public:
  explicit Samplers(const VulkanContext& context);
  ~Samplers();

  Samplers(const Samplers&) = delete;
  Samplers& operator=(const Samplers&) = delete;

  // Material textures: trilinear, repeating, anisotropic when the device
  // supports it.
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

}  // namespace blue::render

#endif  // BLUE_RENDER_SAMPLERS_H_
