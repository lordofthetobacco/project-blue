#ifndef BLUE_RENDER_VULKAN_VK_CHECK_H_
#define BLUE_RENDER_VULKAN_VK_CHECK_H_

#include <vulkan/vulkan.h>

#include <source_location>

namespace blue::render::vulkan {

// Returns normally when `result` is VK_SUCCESS. Otherwise logs the result
// name (string_VkResult) with the file, line and function of the call site,
// then terminates the process through core::log::Fatal. Wrap every Vulkan call
// that returns a VkResult and cannot be recovered from.
//
// `location` defaults to std::source_location::current(), which is evaluated
// at the call site, so callers never pass it.
void CheckVk(VkResult result,
             std::source_location location = std::source_location::current());

}  // namespace blue::render::vulkan

#endif  // BLUE_RENDER_VULKAN_VK_CHECK_H_
