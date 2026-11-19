#include <doctest/doctest.h>

#include <cstddef>

#include "render/texture_types.h"

namespace blue::render {

TEST_CASE("BC mips take whole 4 x 4 blocks of 16 bytes") {
  for (const TextureFormat format :
       {TextureFormat::kBc7Srgb, TextureFormat::kBc7Unorm,
        TextureFormat::kBc5Unorm}) {
    CHECK(MipByteSize(format, 1, 1) == 16);
    CHECK(MipByteSize(format, 4, 4) == 16);
    CHECK(MipByteSize(format, 5, 4) == 32);
    CHECK(MipByteSize(format, 5, 5) == 64);
    CHECK(MipByteSize(format, 2048, 2048) == size_t{512} * 512 * 16);
    CHECK(MipByteSize(format, 2048, 1) == size_t{512} * 16);
  }
}

TEST_CASE("RGBA8 mips take four bytes per texel") {
  CHECK(MipByteSize(TextureFormat::kRgba8Srgb, 1, 1) == 4);
  CHECK(MipByteSize(TextureFormat::kRgba8Unorm, 3, 5) == 60);
}

TEST_CASE("A full chain of a cooked texture ends at 1 x 1") {
  // The mip count Renderer::UploadTextures() accepts at most.
  CHECK(MipLevelCount({2048, 2048}) == 12);
  CHECK(MipLevelCount({2048, 512}) == 12);
  CHECK(MipLevelCount({1, 1}) == 1);
}

}  // namespace blue::render
