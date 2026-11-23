#include "render/samplers.h"

#include <algorithm>

#include "render/vk_check.h"

namespace blue::render {

namespace {

VkSampler CreateSampler(VkDevice device, const VkSamplerCreateInfo& info) {
  VkSampler sampler = VK_NULL_HANDLE;
  CheckVk(vkCreateSampler(device, &info, nullptr, &sampler));
  return sampler;
}

// Returns a sampler with `filter` for both magnification and minification,
// the matching mip mode, `address` on every axis and every mip reachable.
VkSamplerCreateInfo SamplerInfo(VkFilter filter, VkSamplerAddressMode address) {
  return {
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = filter,
      .minFilter = filter,
      .mipmapMode = filter == VK_FILTER_LINEAR ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                               : VK_SAMPLER_MIPMAP_MODE_NEAREST,
      .addressModeU = address,
      .addressModeV = address,
      .addressModeW = address,
      .minLod = 0.0f,
      .maxLod = VK_LOD_CLAMP_NONE,
  };
}

}  // namespace

Samplers::Samplers(const VulkanContext& context, float max_anisotropy)
    : device_(context.device()) {
  VkSamplerCreateInfo material =
      SamplerInfo(VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_REPEAT);
  if (context.sampler_anisotropy() && max_anisotropy > 1.0f) {
    material.anisotropyEnable = VK_TRUE;
    material.maxAnisotropy = std::min(
        max_anisotropy, context.properties().limits.maxSamplerAnisotropy);
  }
  material_ = CreateSampler(device_, material);
  linear_clamp_ = CreateSampler(
      device_,
      SamplerInfo(VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE));
  nearest_clamp_ = CreateSampler(
      device_,
      SamplerInfo(VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE));
}

Samplers::~Samplers() {
  vkDestroySampler(device_, nearest_clamp_, nullptr);
  vkDestroySampler(device_, linear_clamp_, nullptr);
  vkDestroySampler(device_, material_, nullptr);
}

}  // namespace blue::render
