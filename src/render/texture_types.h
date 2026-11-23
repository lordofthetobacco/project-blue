#ifndef BLUE_RENDER_TEXTURE_TYPES_H_
#define BLUE_RENDER_TEXTURE_TYPES_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "core/extent.h"

namespace blue::render {

// Texel encoding of an already-encoded texture (Renderer::UploadTextures()).
// Block-compressed formats store 4 x 4 blocks of 16 bytes; a mip smaller
// than a block still takes one whole block.
enum class TextureFormat : uint8_t {
  kBc7Srgb,     // Colour (base colour, emissive).
  kBc7Unorm,    // Linear data, e.g. packed occlusion/roughness/metallic.
  kBc5Unorm,    // Two channels: tangent-space normal XY, Z reconstructed.
  kRgba8Srgb,   // Uncompressed colour, for tiny or odd-sized textures.
  kRgba8Unorm,  // Uncompressed linear data.
};

// Returns the byte size of one `width` x `height` mip in `format` (whole
// 4 x 4 blocks for BC formats).
[[nodiscard]] size_t MipByteSize(TextureFormat format, uint32_t width,
                                 uint32_t height);

// Returns the mip count of a full chain down to 1 x 1 for `extent`.
[[nodiscard]] uint32_t MipLevelCount(core::Extent2D extent);

// Handle of a texture uploaded with Renderer::UploadTextures(); stays valid
// for the renderer's lifetime.
enum class TextureId : uint32_t {};

// One already-encoded texture with its ready mip chain, for
// Renderer::UploadTextures(). Everything is borrowed for the call.
struct TextureUpload {
  TextureFormat format = TextureFormat::kRgba8Unorm;
  uint32_t width = 0;
  uint32_t height = 0;
  // Level 0 first, each level half the size of the previous one (rounded
  // down, at least 1), each exactly MipByteSize() bytes; 1 to
  // MipLevelCount() levels.
  std::span<const std::span<const std::byte>> mips;
  std::string_view name;  // For logs.
};

// Uploaded textures for the slots of one material. An empty slot gets the
// slot's default (white; flat normal for the normal map). For a cooked
// packed ORM texture (R occlusion, G roughness, B metallic) put it in
// metallic_roughness, leave occlusion empty and set
// MaterialDesc::occlusion_in_metallic_roughness.
struct MaterialTextures {
  std::optional<TextureId> base_color;
  std::optional<TextureId> metallic_roughness;
  std::optional<TextureId> normal;
  std::optional<TextureId> occlusion;
  std::optional<TextureId> emissive;
};

struct TextureStats {
  size_t count = 0;    // Loaded textures, defaults excluded.
  uint64_t bytes = 0;  // Their device memory, mips included.
};

}  // namespace blue::render

#endif  // BLUE_RENDER_TEXTURE_TYPES_H_
