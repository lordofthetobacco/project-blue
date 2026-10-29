#include "render/vk_check.h"

#include <vulkan/vk_enum_string_helper.h>

#include "core/log.h"

namespace blue::render {

void CheckVk(VkResult result, std::source_location location) {
  if (result != VK_SUCCESS) {
    core::log::Fatal("Vulkan call failed: {} ({}:{} in {})",
                     string_VkResult(result), location.file_name(),
                     location.line(), location.function_name());
  }
}

}  // namespace blue::render
