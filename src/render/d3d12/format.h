#ifndef BLUE_RENDER_D3D12_FORMAT_H_
#define BLUE_RENDER_D3D12_FORMAT_H_

#include <dxgiformat.h>

#include <cstddef>
#include <cstdint>
#include <span>

#include "core/extent.h"
#include "render/texture_types.h"

// Texture formats and the layout of texture data in upload buffers. Pure
// functions on DXGI enums: no device needed (unit tested without a GPU; the
// render tests compare the footprints with the device's
// GetCopyableFootprints()).

namespace blue::render::d3d12 {

// Rows of an upload buffer copied into a texture start at multiples of this
// (D3D12_TEXTURE_DATA_PITCH_ALIGNMENT) ...
inline constexpr uint32_t kRowPitchAlignment = 256;
// ... and each subresource's data at a multiple of this
// (D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT).
inline constexpr uint64_t kPlacementAlignment = 512;

// Returns the DXGI format of `format` (the resource's and its views').
[[nodiscard]] DXGI_FORMAT ToDxgiFormat(TextureFormat format);

// The copy layout of a format: rows of blocks, each block `extent` x
// `extent` texels of `bytes` bytes (extent 1 for uncompressed formats).
struct CopyBlock {
  uint32_t extent = 1;
  uint32_t bytes = 0;
};

// Returns the copy layout of `format`.
[[nodiscard]] CopyBlock CopyBlockOf(TextureFormat format);

// Where one mip level of a texture goes in an upload buffer.
struct MipFootprint {
  // The level's size in texels.
  uint32_t width = 0;
  uint32_t height = 0;
  // Rows of blocks, and the bytes of one row when tightly packed (the
  // layout of TextureUpload::mips).
  uint32_t row_count = 0;
  uint32_t row_bytes = 0;
  // Bytes from one row to the next in the upload buffer: row_bytes rounded
  // up to kRowPitchAlignment.
  uint32_t row_pitch = 0;
  // Bytes the level takes in the upload buffer: every row at row_pitch but
  // the last, which is only row_bytes (what GetCopyableFootprints()
  // reports as the level's total).
  uint64_t upload_bytes = 0;
};

// Returns the footprint of mip `level` of a texture with copy layout
// `block` whose mip 0 is `extent`.
[[nodiscard]] MipFootprint MipFootprintOf(CopyBlock block,
                                          core::Extent2D extent,
                                          uint32_t level);
// Same for `format`; row_bytes * row_count equals MipByteSize() of that
// level.
[[nodiscard]] inline MipFootprint MipFootprintOf(TextureFormat format,
                                                 core::Extent2D extent,
                                                 uint32_t level) {
  return MipFootprintOf(CopyBlockOf(format), extent, level);
}

// Copies the tightly packed rows `packed` (row_count * row_bytes bytes) to
// `upload` (at least upload_bytes) at row_pitch. Sizes that do not match
// `footprint` are fatal.
void RepackRows(const MipFootprint& footprint,
                std::span<const std::byte> packed, std::span<std::byte> upload);

// Returns `value` rounded up to a multiple of `alignment` (a power of two).
[[nodiscard]] constexpr uint64_t AlignUp(uint64_t value, uint64_t alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

}  // namespace blue::render::d3d12

#endif  // BLUE_RENDER_D3D12_FORMAT_H_
