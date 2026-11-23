#include <doctest/doctest.h>
#include <vulkan/vulkan.h>

#include "render/texture_types.h"
#include "render/vulkan/texture_cache.h"

namespace blue::render::vulkan {

TEST_CASE("Texture formats map to their Vulkan formats") {
  CHECK(ToVkFormat(TextureFormat::kBc7Srgb) == VK_FORMAT_BC7_SRGB_BLOCK);
  CHECK(ToVkFormat(TextureFormat::kBc7Unorm) == VK_FORMAT_BC7_UNORM_BLOCK);
  CHECK(ToVkFormat(TextureFormat::kBc5Unorm) == VK_FORMAT_BC5_UNORM_BLOCK);
  CHECK(ToVkFormat(TextureFormat::kRgba8Srgb) == VK_FORMAT_R8G8B8A8_SRGB);
  CHECK(ToVkFormat(TextureFormat::kRgba8Unorm) == VK_FORMAT_R8G8B8A8_UNORM);
}

}  // namespace blue::render::vulkan
