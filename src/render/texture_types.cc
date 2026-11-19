#include "render/texture_types.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>

#include "core/log.h"

namespace blue::render {

size_t MipByteSize(TextureFormat format, uint32_t width, uint32_t height) {
  constexpr size_t kBlockSize = 4;
  constexpr size_t kBlockBytes = 16;  // BC5 and BC7 alike.
  constexpr size_t kRgba8Bytes = 4;
  switch (format) {
    case TextureFormat::kBc7Srgb:
    case TextureFormat::kBc7Unorm:
    case TextureFormat::kBc5Unorm:
      return (size_t{width} + kBlockSize - 1) / kBlockSize *
             ((size_t{height} + kBlockSize - 1) / kBlockSize) * kBlockBytes;
    case TextureFormat::kRgba8Srgb:
    case TextureFormat::kRgba8Unorm:
      return size_t{width} * height * kRgba8Bytes;
  }
  core::log::Fatal("Unknown texture format {}", static_cast<int>(format));
}

uint32_t MipLevelCount(core::Extent2D extent) {
  const uint32_t largest = std::max({extent.width, extent.height, 1u});
  return static_cast<uint32_t>(std::bit_width(largest));
}

}  // namespace blue::render
