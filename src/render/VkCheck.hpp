#pragma once

#include "core/Log.hpp"

#include <vulkan/vk_enum_string_helper.h>
#include <vulkan/vulkan.h>

#define VK_CHECK(expr)                                                                       \
    do {                                                                                     \
        const VkResult vkCheckResult_ = (expr);                                              \
        if (vkCheckResult_ != VK_SUCCESS) {                                                  \
            ::blue::log::fatal("{} failed: {} ({}:{})", #expr,                               \
                               string_VkResult(vkCheckResult_), __FILE__, __LINE__);         \
        }                                                                                    \
    } while (false)
