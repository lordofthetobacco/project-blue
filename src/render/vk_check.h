#ifndef BLUE_RENDER_VK_CHECK_H_
#define BLUE_RENDER_VK_CHECK_H_

#include <vulkan/vulkan.h>

#include <source_location>

namespace blue::render {

// Terminates the process through core::log::Fatal, naming the failing call
// site, when `result` is not VK_SUCCESS. Wrap every Vulkan call that returns
// a VkResult and cannot be recovered from.
void CheckVk(VkResult result,
             std::source_location location = std::source_location::current());

}  // namespace blue::render

#endif  // BLUE_RENDER_VK_CHECK_H_
