#ifndef BLUE_RENDER_TEXTURE_CACHE_H_
#define BLUE_RENDER_TEXTURE_CACHE_H_

#include <vulkan/vulkan.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "render/image.h"
#include "render/vulkan_context.h"

namespace blue::render {

// The 1 x 1 textures that stand in for an empty slot or a failed load.
enum class DefaultTexture {
  kWhite,       // 1: base colour, metallic-roughness, occlusion, emissive.
  kFlatNormal,  // (0.5, 0.5, 1): an unperturbed tangent-space normal.
  kBlack,       // 0.
  kCount,
};

// Texel encoding of an already-encoded texture (TextureCache::Upload()).
// Block-compressed formats store 4 x 4 blocks of 16 bytes; a mip smaller
// than a block still takes one whole block.
enum class TextureFormat : uint8_t {
  kBc7Srgb,     // Colour (base colour, emissive).
  kBc7Unorm,    // Linear data, e.g. packed occlusion/roughness/metallic.
  kBc5Unorm,    // Two channels: tangent-space normal XY, Z reconstructed.
  kRgba8Srgb,   // Uncompressed colour, for tiny or odd-sized textures.
  kRgba8Unorm,  // Uncompressed linear data.
};

// Returns the Vulkan format of `format`.
[[nodiscard]] VkFormat ToVkFormat(TextureFormat format);

// Returns the byte size of one `width` x `height` mip in `format` (whole
// 4 x 4 blocks for BC formats).
[[nodiscard]] size_t MipByteSize(TextureFormat format, uint32_t width,
                                 uint32_t height);

// Handle of a texture uploaded with TextureCache::Upload(); stays valid for
// the cache's lifetime.
enum class TextureId : uint32_t {};

// One already-encoded texture with its ready mip chain, for
// TextureCache::Upload(). Everything is borrowed for the call.
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
  size_t count = 0;        // Loaded textures, defaults excluded.
  VkDeviceSize bytes = 0;  // Their device memory, mips included.
};

// The renderer's textures: the 1 x 1 defaults and textures uploaded already
// encoded (Upload()), each resident until taken out with TakeUploaded().
// Nothing is decoded here: textures are cooked into .pbo files. Borrows
// `context`, which must outlive the cache. Neither copyable nor movable.
// Not thread-safe.
class TextureCache {
 public:
  explicit TextureCache(const VulkanContext& context);

  TextureCache(const TextureCache&) = delete;
  TextureCache& operator=(const TextureCache&) = delete;

  // Uploads already-encoded textures with their mip chains as sampled
  // images through bounded staging memory (CreateImagesFromMipChains()), no
  // decoding and no mip generation. Returns their ids in the order of
  // `textures`. Blocks until the copies finished; logs the count, bytes and
  // time. A malformed description (no mips, wrong mip size or count) is
  // fatal: validate file data before.
  [[nodiscard]] std::vector<TextureId> Upload(
      std::span<const TextureUpload> textures);
  // Returns the view of an uploaded texture, or the white default for an
  // unknown or taken id.
  [[nodiscard]] VkImageView view(TextureId id) const {
    const auto index = static_cast<size_t>(id);
    return index < uploaded_.size() &&
                   uploaded_[index].handle() != VK_NULL_HANDLE
               ? uploaded_[index].view()
               : default_view(DefaultTexture::kWhite);
  }
  // Moves uploaded texture `id` out of the cache (for deferred destruction,
  // see Renderer::ReleaseTextures()) and returns it; its slot stays empty
  // and the id is never reused. Returns an empty Image for an unknown or
  // already taken id.
  [[nodiscard]] Image TakeUploaded(TextureId id);
  // Returns the view of `id`, or of `fallback` when `id` is empty.
  [[nodiscard]] VkImageView ViewOr(std::optional<TextureId> id,
                                   DefaultTexture fallback) const {
    return id ? view(*id) : default_view(fallback);
  }

  [[nodiscard]] VkImageView default_view(DefaultTexture texture) const {
    return defaults_[static_cast<size_t>(texture)].view();
  }
  [[nodiscard]] TextureStats stats() const { return stats_; }

 private:
  const VulkanContext& context_;
  std::array<Image, static_cast<size_t>(DefaultTexture::kCount)> defaults_;
  // Textures from Upload(), indexed by TextureId.
  std::vector<Image> uploaded_;
  TextureStats stats_;
};

}  // namespace blue::render

#endif  // BLUE_RENDER_TEXTURE_CACHE_H_
