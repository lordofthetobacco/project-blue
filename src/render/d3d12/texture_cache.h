#ifndef BLUE_RENDER_D3D12_TEXTURE_CACHE_H_
#define BLUE_RENDER_D3D12_TEXTURE_CACHE_H_

#include <d3d12.h>

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include "render/d3d12/image.h"
#include "render/texture_types.h"

namespace blue::render::d3d12 {

class D3d12Context;

// The 1 x 1 textures that stand in for an empty slot or a failed load.
enum class DefaultTexture {
  kWhite,       // 1: base colour, metallic-roughness, occlusion, emissive.
  kFlatNormal,  // (0.5, 0.5, 1): an unperturbed tangent-space normal.
  kBlack,       // 0.
  kCount,
};

// The backend's textures: the 1 x 1 defaults and textures uploaded already
// encoded (Upload()), each resident until taken out with TakeUploaded().
// Every texture has a staging SRV (copied into material tables) and a
// shader-visible one (what ImGui shows: ui_texture()). Between uploads every
// texture is in PIXEL_SHADER_RESOURCE | NON_PIXEL_SHADER_RESOURCE. Borrows
// `context`, which must outlive the cache. Neither copyable nor movable.
// Not thread-safe.
class TextureCache {
 public:
  explicit TextureCache(const D3d12Context& context);

  TextureCache(const TextureCache&) = delete;
  TextureCache& operator=(const TextureCache&) = delete;

  // Uploads already-encoded textures with their mip chains through bounded
  // upload memory (StagingUploader), no decoding and no mip generation.
  // Returns their ids in the order of `textures`. Blocks until the copies
  // finished; logs the count, bytes and time. A malformed description (no
  // mips, wrong mip size or count) is fatal.
  [[nodiscard]] std::vector<TextureId> Upload(
      std::span<const TextureUpload> textures);
  // Moves uploaded texture `id` out of the cache (for deferred destruction)
  // and returns it; its slot stays empty and the id is never reused.
  // Returns an empty Image for an unknown or already taken id.
  [[nodiscard]] Image TakeUploaded(TextureId id);

  // The image of uploaded texture `id`, or the white default for an unknown
  // or taken id.
  [[nodiscard]] const Image& image(TextureId id) const {
    const auto index = static_cast<size_t>(id);
    return index < uploaded_.size() && uploaded_[index].valid()
               ? uploaded_[index]
               : default_image(DefaultTexture::kWhite);
  }
  // The image of `id`, or of `fallback` when `id` is empty.
  [[nodiscard]] const Image& ImageOr(std::optional<TextureId> id,
                                     DefaultTexture fallback) const {
    return id ? image(*id) : default_image(fallback);
  }
  [[nodiscard]] const Image& default_image(DefaultTexture texture) const {
    return defaults_[static_cast<size_t>(texture)];
  }
  [[nodiscard]] TextureStats stats() const { return stats_; }

 private:
  const D3d12Context& context_;
  std::array<Image, static_cast<size_t>(DefaultTexture::kCount)> defaults_;
  // Textures from Upload(), indexed by TextureId.
  std::vector<Image> uploaded_;
  TextureStats stats_;
};

}  // namespace blue::render::d3d12

#endif  // BLUE_RENDER_D3D12_TEXTURE_CACHE_H_
