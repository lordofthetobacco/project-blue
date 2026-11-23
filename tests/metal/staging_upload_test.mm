#include "render/metal/staging_upload.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "core/extent.h"
#include "render/metal/texture_cache.h"
#include "render/texture_types.h"

// Objective-C++ because the copy layouts are those of Metal pixel formats
// (render/staging_upload.h is an internal render header).

namespace blue::render::metal {

TEST_CASE("Copy blocks of upload formats") {
  CHECK(CopyBlockOf(MTLPixelFormatBC7_RGBAUnorm_sRGB).extent == 4);
  CHECK(CopyBlockOf(MTLPixelFormatBC7_RGBAUnorm_sRGB).bytes == 16);
  CHECK(CopyBlockOf(MTLPixelFormatBC5_RGUnorm).bytes == 16);
  CHECK(CopyBlockOf(MTLPixelFormatBC1_RGBA).bytes == 8);
  CHECK(CopyBlockOf(MTLPixelFormatRGBA8Unorm_sRGB).extent == 1);
  CHECK(CopyBlockOf(MTLPixelFormatRGBA8Unorm_sRGB).bytes == 4);
  CHECK(CopyBlockOf(MTLPixelFormatRGBA32Float).bytes == 16);
  CHECK(CopyBlockOf(MTLPixelFormatDepth32Float).bytes == 0);
}

TEST_CASE("The staging copy layout matches the cooked mip sizes") {
  // StagingUploader::CopyToImage() splits levels by rows of blocks; a level
  // that TextureCache::Upload() accepts must be exactly that many bytes.
  for (const TextureFormat format :
       {TextureFormat::kBc7Srgb, TextureFormat::kBc7Unorm,
        TextureFormat::kBc5Unorm, TextureFormat::kRgba8Srgb,
        TextureFormat::kRgba8Unorm}) {
    for (const core::Extent2D extent :
         {core::Extent2D{4096, 4096}, core::Extent2D{1000, 300},
          core::Extent2D{5, 3}, core::Extent2D{1, 1}}) {
      for (uint32_t level = 0; level < MipLevelCount(extent); ++level) {
        const uint32_t width = std::max(extent.width >> level, 1u);
        const uint32_t height = std::max(extent.height >> level, 1u);
        CHECK(CopyByteSize(ToPixelFormat(format), extent, level) ==
              MipByteSize(format, width, height));
      }
    }
  }
}

TEST_CASE("Staging chunks stay far below a 4k texture batch") {
  // Two chunks in turn bound the staging memory of any upload; a cooked
  // 4096 x 4096 BC7 level alone is 16 MiB.
  CHECK(2 * StagingUploader::kChunkBytes <= size_t{32} << 20);
  CHECK(StagingUploader::kChunkBytes >=
        CopyByteSize(MTLPixelFormatBC7_RGBAUnorm_sRGB, {4096, 4096}, 0));
}

}  // namespace blue::render::metal
