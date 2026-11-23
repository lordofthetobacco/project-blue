#include <doctest/doctest.h>
#include <dxgiformat.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/extent.h"
#include "render/d3d12/format.h"
#include "render/texture_types.h"

namespace blue::render::d3d12 {
namespace {

constexpr std::array kFormats = {
    TextureFormat::kBc7Srgb,   TextureFormat::kBc7Unorm,
    TextureFormat::kBc5Unorm,  TextureFormat::kRgba8Srgb,
    TextureFormat::kRgba8Unorm,
};

TEST_CASE("Texture formats map to their DXGI formats") {
  CHECK(ToDxgiFormat(TextureFormat::kBc7Srgb) == DXGI_FORMAT_BC7_UNORM_SRGB);
  CHECK(ToDxgiFormat(TextureFormat::kBc7Unorm) == DXGI_FORMAT_BC7_UNORM);
  CHECK(ToDxgiFormat(TextureFormat::kBc5Unorm) == DXGI_FORMAT_BC5_UNORM);
  CHECK(ToDxgiFormat(TextureFormat::kRgba8Srgb) ==
        DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
  CHECK(ToDxgiFormat(TextureFormat::kRgba8Unorm) == DXGI_FORMAT_R8G8B8A8_UNORM);
}

TEST_CASE("Mip footprints hold exactly MipByteSize() at an aligned pitch") {
  // Odd, non-square and tiny sizes; every level of each chain.
  constexpr std::array<core::Extent2D, 6> kExtents = {{
      {1, 1}, {3, 5}, {64, 64}, {100, 37}, {640, 360}, {1024, 1},
  }};
  for (const TextureFormat format : kFormats) {
    for (const core::Extent2D extent : kExtents) {
      const uint32_t levels = MipLevelCount(extent);
      for (uint32_t level = 0; level < levels; ++level) {
        CAPTURE(static_cast<int>(format));
        CAPTURE(extent.width);
        CAPTURE(extent.height);
        CAPTURE(level);
        const MipFootprint footprint = MipFootprintOf(format, extent, level);
        CHECK(footprint.width == std::max(extent.width >> level, 1u));
        CHECK(footprint.height == std::max(extent.height >> level, 1u));
        CHECK(size_t{footprint.row_bytes} * footprint.row_count ==
              MipByteSize(format, footprint.width, footprint.height));
        CHECK(footprint.row_pitch % kRowPitchAlignment == 0);
        CHECK(footprint.row_pitch >= footprint.row_bytes);
        CHECK(footprint.row_pitch < footprint.row_bytes + kRowPitchAlignment);
        CHECK(footprint.upload_bytes ==
              uint64_t{footprint.row_pitch} * (footprint.row_count - 1) +
                  footprint.row_bytes);
      }
    }
  }
}

TEST_CASE("Block-compressed footprints count whole blocks") {
  // A 1 x 1 BC7 mip still takes one 16-byte block.
  const MipFootprint tiny = MipFootprintOf(TextureFormat::kBc7Srgb, {1, 1}, 0);
  CHECK(tiny.row_count == 1);
  CHECK(tiny.row_bytes == 16);
  CHECK(tiny.row_pitch == 256);
  CHECK(tiny.upload_bytes == 16);
  // 100 x 37: 25 x 10 blocks.
  const MipFootprint odd = MipFootprintOf(TextureFormat::kBc5Unorm, {100, 37}, 0);
  CHECK(odd.row_count == 10);
  CHECK(odd.row_bytes == 25 * 16);
  CHECK(odd.row_pitch == 512);
  // RGBA8 rows of 640 texels are already aligned (2560 bytes).
  const MipFootprint wide =
      MipFootprintOf(TextureFormat::kRgba8Unorm, {640, 360}, 0);
  CHECK(wide.row_pitch == wide.row_bytes);
  CHECK(wide.upload_bytes == size_t{640} * 360 * 4);
}

TEST_CASE("Repacking rows keeps every byte at its pitched place") {
  const MipFootprint footprint =
      MipFootprintOf(TextureFormat::kRgba8Unorm, {5, 3}, 0);
  REQUIRE(footprint.row_bytes == 20);
  REQUIRE(footprint.row_pitch == 256);
  std::vector<std::byte> packed(size_t{footprint.row_bytes} *
                                footprint.row_count);
  for (size_t i = 0; i < packed.size(); ++i) {
    packed[i] = static_cast<std::byte>(i + 1);
  }
  std::vector<std::byte> upload(footprint.upload_bytes, std::byte{0xEE});
  RepackRows(footprint, packed, upload);
  for (uint32_t row = 0; row < footprint.row_count; ++row) {
    for (uint32_t i = 0; i < footprint.row_pitch; ++i) {
      const size_t at = size_t{row} * footprint.row_pitch + i;
      if (at >= upload.size()) {
        break;
      }
      if (i < footprint.row_bytes) {
        CHECK(upload[at] == packed[size_t{row} * footprint.row_bytes + i]);
      } else {
        CHECK(upload[at] == std::byte{0xEE});  // Padding is not written.
      }
    }
  }
}

TEST_CASE("AlignUp rounds up to powers of two") {
  CHECK(AlignUp(0, 256) == 0);
  CHECK(AlignUp(1, 256) == 256);
  CHECK(AlignUp(256, 256) == 256);
  CHECK(AlignUp(257, 512) == 512);
}

}  // namespace
}  // namespace blue::render::d3d12
