#ifndef BLUE_RENDER_METAL_IMAGE_H_
#define BLUE_RENDER_METAL_IMAGE_H_

#include "render/metal/metal_types.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "core/extent.h"
#include "render/metal/metal_context.h"

namespace blue::render::metal {

struct ImageDesc {
  core::Extent2D extent{};
  MTLPixelFormat format = MTLPixelFormatInvalid;
  MTLTextureUsage usage = MTLTextureUsageShaderRead;
  // More than one layer makes a 2D array texture; a cube has 6.
  uint32_t array_layers = 1;
  // 1 = no mips; MipLevelCount(extent) for a full chain.
  uint32_t mip_levels = 1;
  // A cube texture: array_layers must be 6 and the extent square.
  bool cube = false;
  // Names the texture in GPU captures; nullptr gives a generic name.
  const char* label = nullptr;
};

// Returns the size of one texel of `format` in bytes, or 0 for formats the
// upload path does not handle (block-compressed, depth, packed).
[[nodiscard]] uint32_t TexelSize(MTLPixelFormat format);

// Which part of an image a view covers. A zero count means "all remaining".
// For a cube image the layers are its faces.
struct ImageViewDesc {
  MTLTextureType type = MTLTextureType2D;
  uint32_t base_mip = 0;
  uint32_t mip_count = 1;
  uint32_t base_layer = 0;
  uint32_t layer_count = 1;
};

// A texture view of part of an image, e.g. one mip to sample on its own. It
// keeps the image's memory alive, but not resident: it must not be used by
// the GPU after its image was destroyed. Move-only; a moved-from view is empty. Not
// thread-safe.
class ImageView {
 public:
  ImageView() = default;
  explicit ImageView(id<MTLTexture> texture) : texture_(texture) {}

  ImageView(const ImageView&) = delete;
  ImageView& operator=(const ImageView&) = delete;
  ImageView(ImageView&&) = default;
  ImageView& operator=(ImageView&&) = default;

  [[nodiscard]] id<MTLTexture> handle() const { return texture_; }

 private:
  id<MTLTexture> texture_ = nil;
};

// A private 2D texture (single, 2D array or cube), resident on the context's
// queue for its lifetime (see MetalContext). Rendering to one layer or
// mip uses the render pass attachment's `slice` / `level` on texture().
// Move-only; a moved-from image is empty (nil texture, every accessor 0). Not
// thread-safe.
class Image {
 public:
  Image() = default;
  // Creates the texture on `context`, which must outlive it; a cube desc
  // without 6 layers and a square extent, or a device failure, is fatal.
  Image(const MetalContext& context, const ImageDesc& desc);

  Image(const Image&) = delete;
  Image& operator=(const Image&) = delete;
  Image(Image&&) = default;
  Image& operator=(Image&&) = default;

  // Returns the texture of every mip and layer: 2D for a single layer, 2D
  // array for an array image, cube for a cube image.
  [[nodiscard]] id<MTLTexture> texture() const { return texture_; }
  // Creates a view of part of this image, with the image's format.
  [[nodiscard]] ImageView CreateView(const ImageViewDesc& desc) const;

  [[nodiscard]] MTLPixelFormat format() const;
  [[nodiscard]] core::Extent2D extent() const;
  // Layers of the image; 6 for a cube.
  [[nodiscard]] uint32_t array_layers() const;
  [[nodiscard]] uint32_t mip_levels() const;
  // GPU memory used by the image, in bytes.
  [[nodiscard]] uint64_t size_bytes() const;

 private:
  // Every property is read from the texture itself, so that the defaulted
  // moves leave the moved-from image empty.
  id<MTLTexture> texture_ = nil;
  Resident resident_;
};

// Creates a sampled color image from CPU texels and blocks until it is ready.
// `texels` holds mip 0 of every layer, tightly packed, layer after layer
// (TexelSize(desc.format) bytes per texel, rows top to bottom); its size must
// match exactly. When desc.mip_levels > 1 the other mips are generated on
// the GPU (generateMipmapsForTexture); when the format cannot be filtered
// there the image gets a single mip and a warning is logged. ShaderRead is
// added to desc.usage. A size mismatch is fatal.
[[nodiscard]] Image CreateSampledImage(const MetalContext& context,
                                       ImageDesc desc,
                                       std::span<const std::byte> texels);

// One image of a CreateImagesFromMipChains() batch.
struct MipChainUpload {
  // A single-layer 2D image; mip_levels must equal mips.size().
  ImageDesc desc;
  // The bytes of each level, level 0 first, in the copy layout of
  // desc.format (tightly packed rows top to bottom, or rows of 4 x 4 blocks
  // of 16 bytes for BC5 / BC7). Borrowed for the call.
  std::span<const std::span<const std::byte>> mips;
};

// Creates sampled images from ready mip chains, block-compressed formats
// (BC5, BC7) included, with no mip generation: the levels stream through
// bounded staging memory (StagingUploader: submissions of at most 16 MiB
// each, never a staging buffer of the whole batch); blocks until the copies
// finished. Level sizes are the caller's contract (see
// TextureCache::Upload(), which validates them); a level of the wrong size
// is fatal. ShaderRead is added to the usage. Returns the images in the
// order of `uploads`.
[[nodiscard]] std::vector<Image> CreateImagesFromMipChains(
    const MetalContext& context, std::span<const MipChainUpload> uploads);

}  // namespace blue::render::metal

#endif  // BLUE_RENDER_METAL_IMAGE_H_
