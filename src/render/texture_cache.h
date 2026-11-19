#ifndef BLUE_RENDER_TEXTURE_CACHE_H_
#define BLUE_RENDER_TEXTURE_CACHE_H_

#include "render/metal_types.h"

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include "render/image.h"
#include "render/metal_context.h"
#include "render/texture_types.h"

namespace blue::render {

// The 1 x 1 textures that stand in for an empty slot or a failed load.
enum class DefaultTexture {
  kWhite,       // 1: base colour, metallic-roughness, occlusion, emissive.
  kFlatNormal,  // (0.5, 0.5, 1): an unperturbed tangent-space normal.
  kBlack,       // 0.
  kCount,
};

// Returns the Metal pixel format of `format`.
[[nodiscard]] MTLPixelFormat ToPixelFormat(TextureFormat format);

// The renderer's textures: the 1 x 1 defaults and textures uploaded already
// encoded (Upload()), each resident until taken out with TakeUploaded().
// Nothing is decoded here: textures are cooked into .pbo files. Borrows
// `context`, which must outlive the cache. Neither copyable nor movable.
// Not thread-safe.
class TextureCache {
 public:
  explicit TextureCache(const MetalContext& context);

  TextureCache(const TextureCache&) = delete;
  TextureCache& operator=(const TextureCache&) = delete;

  // Uploads already-encoded textures with their mip chains as sampled
  // images: one staging buffer and one submission for the whole batch, no
  // decoding and no mip generation. Returns their ids in the order of
  // `textures`. Blocks until the copies finished; logs the count, bytes and
  // time. A malformed description (no mips, wrong mip size or count) is
  // fatal: validate file data before.
  [[nodiscard]] std::vector<TextureId> Upload(
      std::span<const TextureUpload> textures);
  // Returns the texture of an uploaded texture id, or the white default for
  // an unknown or taken id.
  [[nodiscard]] id<MTLTexture> texture(TextureId id) const {
    const auto index = static_cast<size_t>(id);
    return index < uploaded_.size() && uploaded_[index].texture() != nil
               ? uploaded_[index].texture()
               : default_texture(DefaultTexture::kWhite);
  }
  // Moves uploaded texture `id` out of the cache (for deferred destruction,
  // see Renderer::ReleaseTextures()) and returns it; its slot stays empty
  // and the id is never reused. Returns an empty Image for an unknown or
  // already taken id.
  [[nodiscard]] Image TakeUploaded(TextureId id);
  // Returns the texture of `id`, or `fallback` when `id` is empty.
  [[nodiscard]] id<MTLTexture> TextureOr(std::optional<TextureId> id,
                                         DefaultTexture fallback) const {
    return id ? texture(*id) : default_texture(fallback);
  }

  [[nodiscard]] id<MTLTexture> default_texture(DefaultTexture texture) const {
    return defaults_[static_cast<size_t>(texture)].texture();
  }
  [[nodiscard]] TextureStats stats() const { return stats_; }

 private:
  const MetalContext& context_;
  std::array<Image, static_cast<size_t>(DefaultTexture::kCount)> defaults_;
  // Textures from Upload(), indexed by TextureId; a taken slot holds an
  // empty Image (the tombstone).
  std::vector<Image> uploaded_;
  TextureStats stats_;
};

}  // namespace blue::render

#endif  // BLUE_RENDER_TEXTURE_CACHE_H_
