#include "render/d3d12/format.h"

#include <dxgiformat.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "core/extent.h"
#include "core/log.h"
#include "render/texture_types.h"

namespace blue::render::d3d12 {

DXGI_FORMAT ToDxgiFormat(TextureFormat format) {
  switch (format) {
    case TextureFormat::kBc7Srgb:
      return DXGI_FORMAT_BC7_UNORM_SRGB;
    case TextureFormat::kBc7Unorm:
      return DXGI_FORMAT_BC7_UNORM;
    case TextureFormat::kBc5Unorm:
      return DXGI_FORMAT_BC5_UNORM;
    case TextureFormat::kRgba8Srgb:
      return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case TextureFormat::kRgba8Unorm:
      return DXGI_FORMAT_R8G8B8A8_UNORM;
  }
  return DXGI_FORMAT_UNKNOWN;
}

CopyBlock CopyBlockOf(TextureFormat format) {
  switch (format) {
    case TextureFormat::kBc7Srgb:
    case TextureFormat::kBc7Unorm:
    case TextureFormat::kBc5Unorm:
      return {.extent = 4, .bytes = 16};
    case TextureFormat::kRgba8Srgb:
    case TextureFormat::kRgba8Unorm:
      return {.extent = 1, .bytes = 4};
  }
  return {};
}

MipFootprint MipFootprintOf(CopyBlock block, core::Extent2D extent,
                            uint32_t level) {
  MipFootprint footprint;
  footprint.width = std::max(extent.width >> level, 1u);
  footprint.height = std::max(extent.height >> level, 1u);
  const uint32_t blocks_wide = (footprint.width + block.extent - 1) / block.extent;
  footprint.row_count = (footprint.height + block.extent - 1) / block.extent;
  footprint.row_bytes = blocks_wide * block.bytes;
  footprint.row_pitch =
      static_cast<uint32_t>(AlignUp(footprint.row_bytes, kRowPitchAlignment));
  footprint.upload_bytes =
      uint64_t{footprint.row_pitch} * (footprint.row_count - 1) +
      footprint.row_bytes;
  return footprint;
}

void RepackRows(const MipFootprint& footprint,
                std::span<const std::byte> packed,
                std::span<std::byte> upload) {
  const size_t packed_bytes = size_t{footprint.row_bytes} * footprint.row_count;
  if (packed.size() != packed_bytes || upload.size() < footprint.upload_bytes) {
    core::log::Fatal("Texture rows: {} bytes given for {} rows of {} bytes, {} "
                     "bytes to write them into ({} needed)",
                     packed.size(), footprint.row_count, footprint.row_bytes,
                     upload.size(), footprint.upload_bytes);
  }
  if (footprint.row_pitch == footprint.row_bytes) {
    std::memcpy(upload.data(), packed.data(), packed_bytes);
    return;
  }
  for (uint32_t row = 0; row < footprint.row_count; ++row) {
    std::memcpy(upload.data() + size_t{row} * footprint.row_pitch,
                packed.data() + size_t{row} * footprint.row_bytes,
                footprint.row_bytes);
  }
}

}  // namespace blue::render::d3d12
