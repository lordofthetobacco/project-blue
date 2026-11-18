#ifndef BLUE_RENDER_TEXTURE_CACHE_H_
#define BLUE_RENDER_TEXTURE_CACHE_H_

#include <vulkan/vulkan.h>

#include <array>
#include <cstddef>
#include <functional>
#include <span>
#include <unordered_map>

#include "material/material.h"
#include "render/image.h"
#include "render/vulkan_context.h"

namespace blue::render {

// How a texture's texels are interpreted.
enum class TextureKind {
  kColor,  // sRGB-encoded colour (base colour, emissive): an _SRGB format.
  kData,   // Linear data (normals, occlusion, metallic-roughness): _UNORM.
  // Separate greyscale roughness and metallic maps packed into one glTF
  // metallic-roughness texture (G roughness, B metallic): _UNORM.
  kPackedMetallicRoughness,
};

// The 1 x 1 textures that stand in for an empty slot or a failed load.
enum class DefaultTexture {
  kWhite,       // 1: base colour, metallic-roughness, occlusion, emissive.
  kFlatNormal,  // (0.5, 0.5, 1): an unperturbed tangent-space normal.
  kBlack,       // 0.
  kCount,
};

struct TextureStats {
  size_t count = 0;        // Loaded textures, defaults excluded.
  VkDeviceSize bytes = 0;  // Their device memory, mips included.
};

// Decodes material textures (asset::LoadTextures) and uploads them as
// sampled images with full mip chains, once per (source, kind). A texture
// that fails to load is logged once and served as its default from then on.
// Textures stay resident for the cache's lifetime. Borrows `context`, which
// must outlive the cache. Neither copyable nor movable. Not thread-safe.
class TextureCache {
 public:
  explicit TextureCache(const VulkanContext& context);

  TextureCache(const TextureCache&) = delete;
  TextureCache& operator=(const TextureCache&) = delete;

  // Loads every texture `materials` reference that is not cached yet:
  // decoding runs on worker threads (each model file is parsed once), the
  // uploads on this thread. Logs the count and time. Optional: Get() loads
  // on demand, but one batch is much faster for a model's materials.
  void Preload(std::span<const material::MaterialDesc> materials);

  // Returns the view of `ref` as `kind` (kColor or kData), loading it on
  // first use, or the view of `fallback` when `ref` is empty or failed.
  [[nodiscard]] VkImageView Get(const material::TextureRef& ref,
                                TextureKind kind, DefaultTexture fallback);

  // Returns the metallic-roughness view of `desc`: its packed texture when
  // set, else its separate roughness / metallic maps packed into one (see
  // asset::PackMetallicRoughness()), else white.
  [[nodiscard]] VkImageView GetMetallicRoughness(
      const material::MaterialDesc& desc);

  [[nodiscard]] VkImageView default_view(DefaultTexture texture) const {
    return defaults_[static_cast<size_t>(texture)].view();
  }
  [[nodiscard]] TextureStats stats() const { return stats_; }

 private:
  // Identifies one uploaded texture: `primary` (and, for packed
  // metallic-roughness, `secondary` = metallic; primary = roughness) in a
  // kind.
  struct Key {
    material::TextureRef primary;
    material::TextureRef secondary;
    TextureKind kind = TextureKind::kColor;

    friend bool operator==(const Key&, const Key&) = default;
  };
  struct KeyHash {
    [[nodiscard]] size_t operator()(const Key& key) const;
  };

  // Decodes and uploads every key of `keys` that is not cached yet.
  void Load(std::span<const Key> keys);
  // Returns the view of the cached `key`, or `fallback` if it failed.
  [[nodiscard]] VkImageView ViewOf(const Key& key,
                                   DefaultTexture fallback) const;

  const VulkanContext& context_;
  std::array<Image, static_cast<size_t>(DefaultTexture::kCount)> defaults_;
  // An empty Image marks a failed load.
  std::unordered_map<Key, Image, KeyHash> textures_;
  TextureStats stats_;
};

}  // namespace blue::render

#endif  // BLUE_RENDER_TEXTURE_CACHE_H_
