#ifndef BLUE_RENDER_VK_EXTENT_H_
#define BLUE_RENDER_VK_EXTENT_H_

#include <vulkan/vulkan.h>

#include "core/extent.h"

namespace blue::render {

// Conversions at the boundary between the public API (core::Extent2D) and
// the Vulkan internals (VkExtent2D).
// TODO(metal-stage2): drop with the Vulkan backend.
[[nodiscard]] inline VkExtent2D ToVkExtent(core::Extent2D extent) {
  return {extent.width, extent.height};
}

[[nodiscard]] inline core::Extent2D ToExtent(VkExtent2D extent) {
  return {extent.width, extent.height};
}

}  // namespace blue::render

#endif  // BLUE_RENDER_VK_EXTENT_H_
