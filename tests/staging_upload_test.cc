#include "render/staging_upload.h"

#include <doctest/doctest.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdint>

#include "render/image.h"
#include "render/texture_cache.h"

namespace blue::render {

TEST_CASE("Copy blocks of upload formats") {
  CHECK(CopyBlockOf(VK_FORMAT_BC7_SRGB_BLOCK).extent == 4);
  CHECK(CopyBlockOf(VK_FORMAT_BC7_SRGB_BLOCK).bytes == 16);
  CHECK(CopyBlockOf(VK_FORMAT_BC5_UNORM_BLOCK).bytes == 16);
  CHECK(CopyBlockOf(VK_FORMAT_BC1_RGB_UNORM_BLOCK).bytes == 8);
  CHECK(CopyBlockOf(VK_FORMAT_R8G8B8A8_SRGB).extent == 1);
  CHECK(CopyBlockOf(VK_FORMAT_R8G8B8A8_SRGB).bytes == 4);
  CHECK(CopyBlockOf(VK_FORMAT_R32G32B32A32_SFLOAT).bytes == 16);
  CHECK(CopyBlockOf(VK_FORMAT_D32_SFLOAT).bytes == 0);
}

TEST_CASE("The staging copy layout matches the cooked mip sizes") {
  // StagingUploader::CopyToImage() splits levels by rows of blocks; a level
  // that TextureCache::Upload() accepts must be exactly that many bytes.
  for (const TextureFormat format :
       {TextureFormat::kBc7Srgb, TextureFormat::kBc7Unorm,
        TextureFormat::kBc5Unorm, TextureFormat::kRgba8Srgb,
        TextureFormat::kRgba8Unorm}) {
    for (const VkExtent2D extent :
         {VkExtent2D{4096, 4096}, VkExtent2D{1000, 300}, VkExtent2D{5, 3},
          VkExtent2D{1, 1}}) {
      for (uint32_t level = 0; level < MipLevelCount(extent); ++level) {
        const uint32_t width = std::max(extent.width >> level, 1u);
        const uint32_t height = std::max(extent.height >> level, 1u);
        CHECK(CopyByteSize(ToVkFormat(format), extent, level) ==
              MipByteSize(format, width, height));
      }
    }
  }
}

TEST_CASE("Staging chunks stay far below a 4k texture batch") {
  // Two chunks in turn bound the staging memory of any upload; a cooked
  // 4096 x 4096 BC7 level alone is 16 MiB.
  CHECK(2 * StagingUploader::kChunkBytes <= VkDeviceSize{32} << 20);
  CHECK(StagingUploader::kChunkBytes >=
        CopyByteSize(VK_FORMAT_BC7_SRGB_BLOCK, {4096, 4096}, 0));
}

}  // namespace blue::render
