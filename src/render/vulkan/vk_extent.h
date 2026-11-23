#ifndef BLUE_RENDER_VULKAN_VK_EXTENT_H_
#define BLUE_RENDER_VULKAN_VK_EXTENT_H_

#include <vulkan/vulkan.h>

#include "core/extent.h"

namespace blue::render::vulkan {

// Conversions between the API-neutral core::Extent2D of the public renderer
// interface and Vulkan's VkExtent2D (the same two uint32_t).
[[nodiscard]] inline VkExtent2D ToVkExtent(core::Extent2D extent) {
  return {extent.width, extent.height};
}
[[nodiscard]] inline core::Extent2D ToExtent(VkExtent2D extent) {
  return {extent.width, extent.height};
}

}  // namespace blue::render::vulkan

#endif  // BLUE_RENDER_VULKAN_VK_EXTENT_H_
