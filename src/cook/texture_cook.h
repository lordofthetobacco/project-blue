#ifndef BLUE_COOK_TEXTURE_COOK_H_
#define BLUE_COOK_TEXTURE_COOK_H_

#include <cstddef>
#include <vector>

#include "asset/image_decoder.h"
#include "cook/cook_error.h"
#include "core/result.h"
#include "prefab/prefab.h"

// Texture cooking: CPU mip generation and BC7 / BC5 compression into the
// GPU-ready prefab::Texture form. Compression runs on all hardware threads.
// Output is deterministic: the same image and options give the same bytes.

namespace blue::cook {

// What a texture holds; decides colour space, mip filtering and format.
enum class TextureUsage {
  // sRGB colour (base colour, emissive). Mips are averaged in linear light.
  // BC7 sRGB, or RGBA8 sRGB uncompressed.
  kColor,
  // Linear data (the packed ORM texture, masks). BC7 UNORM, or RGBA8 UNORM.
  kLinear,
  // Tangent-space normal map, +Y up, xyz in RGB. Mips average the decoded
  // vectors and renormalise them. BC5 (x, y; z is reconstructed by the
  // shader), or RGBA8 UNORM keeping xyz.
  kNormal,
};

// Encoder effort: kFast for iteration, kHigh for final content.
enum class CookQuality { kFast, kHigh };

struct TextureCookOptions {
  TextureUsage usage = TextureUsage::kColor;
  CookQuality quality = CookQuality::kHigh;
  // false keeps 8-bit RGBA (for tiny textures or debugging).
  bool compress = true;
  // false stores level 0 only.
  bool generate_mips = true;
  // > 0: the base level is halved (box filter) until both sides fit.
  int max_size = 0;
};

// Returns the full mip chain of `image`, level 0 first (a copy of `image`, or
// a reduced copy when max_size requires it) down to 1 x 1. Level i is
// max(1, width >> i) x max(1, height >> i). Every level is filtered from the
// previous one with an exact box filter (odd sizes weight the covered texels
// by their overlap), in linear light for kColor and on renormalised vectors
// for kNormal; alpha is averaged linearly. `image` must hold width * height
// RGBA8 pixels with positive dimensions.
[[nodiscard]] std::vector<asset::ImageData> GenerateMipChain(
    const asset::ImageData& image, TextureUsage usage, int max_size = 0);

// Cooks `image` into a texture of `options.usage`, storing the mips in one
// new block of `storage` (each level kBulkAlignment-aligned). The texture
// has no name. BC levels smaller than a block, and block edges past odd
// sizes, are padded by repeating the last row and column. Fails with
// kInvalidImage for an empty image or a pixel buffer that does not match the
// dimensions, and kTooLarge above asset::kMaxImageDimension. Logs the time.
[[nodiscard]] core::Result<prefab::Texture, CookError> CookTexture(
    const asset::ImageData& image, const TextureCookOptions& options,
    prefab::BulkStorage& storage);

// A texture encoded by EncodeTexture() but not stored yet: `texture` is
// complete except that its mips have no data; mip i is the
// MipByteSize() bytes at mip_offsets[i] of `bytes`.
struct EncodedTexture {
  prefab::Texture texture;
  std::vector<size_t> mip_offsets;
  std::vector<std::byte> bytes;
};

// CookTexture() in two steps, for callers that cook several textures on
// different threads and store them in a fixed order. EncodeTexture() takes
// `image` over (pass it with std::move to avoid a copy) and filters and
// encodes one level at a time: besides `image` it holds at most the next
// level as floats (half the source's size) and the output. Same errors and
// bytes as CookTexture(). Thread-safe for different images.
[[nodiscard]] core::Result<EncodedTexture, CookError> EncodeTexture(
    asset::ImageData image, const TextureCookOptions& options);

// Moves the bytes of `encoded` into one new block of `storage` and returns
// the texture with its mips pointing into it.
[[nodiscard]] prefab::Texture StoreTexture(EncodedTexture encoded,
                                           prefab::BulkStorage& storage);

// Optional inputs of a packed ORM texture (R occlusion, G roughness,
// B metallic), each read from one channel (0 = R ... 3 = A) of its image.
// A null image gives that channel 255, so the material factor applies
// unchanged. The images may be the same object (a glTF metallic-roughness
// texture provides G and B).
struct OrmSources {
  const asset::ImageData* occlusion = nullptr;
  int occlusion_channel = 0;
  const asset::ImageData* roughness = nullptr;
  int roughness_channel = 0;
  const asset::ImageData* metallic = nullptr;
  int metallic_channel = 0;
};

// Packs `sources` into one RGBA8 image with alpha 255, the size of the
// largest input (smaller inputs are resampled bilinearly). Returns an empty
// image if every input is null.
[[nodiscard]] asset::ImageData PackOrm(const OrmSources& sources);

// Decodes mip `mip` of `texture` back to RGBA8: BC7 to RGBA, BC5 to
// (x, y, reconstructed z, 255) encoded as unsigned normals, RGBA8 as is.
// Meant for tests and editor previews. Returns an empty image for an invalid
// mip index.
[[nodiscard]] asset::ImageData DecodeTextureMip(const prefab::Texture& texture,
                                                int mip);

}  // namespace blue::cook

#endif  // BLUE_COOK_TEXTURE_COOK_H_
