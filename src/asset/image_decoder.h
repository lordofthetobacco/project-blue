#ifndef BLUE_ASSET_IMAGE_DECODER_H_
#define BLUE_ASSET_IMAGE_DECODER_H_

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "core/result.h"

// Image decoding for textures and environment maps (stb_image): PNG (8 and
// 16 bit), JPEG, BMP, TGA and Radiance .hdr. Pixels are never flipped: row 0
// is the top of the image, matching the UV convention of asset::Vertex.

namespace blue::asset {

// An 8-bit RGBA image, rows top to bottom. 16-bit sources are reduced to 8
// bits; images without alpha get alpha 255; grey images are replicated to
// RGB.
struct ImageData {
  int width = 0;
  int height = 0;
  int channels = 4;             // always 4
  std::vector<uint8_t> pixels;  // width * height * 4 bytes
};

// A linear floating-point RGB image (HDR), rows top to bottom.
struct ImageDataF {
  int width = 0;
  int height = 0;
  std::vector<float> pixels;  // width * height * 3 floats
};

enum class ImageError {
  kFileNotFound,  // The file cannot be read.
  kDecodeFailed,  // The data is not an image stb_image can decode.
  kTooLarge,      // The image exceeds kMaxImageDimension.
  kNoTexture,     // The TextureRef is empty or names no embedded texture
                  // (asset/texture_loader.h).
};

// Largest accepted width or height.
inline constexpr int kMaxImageDimension = 16384;

// Returns a short human-readable name for `error`, for log messages.
[[nodiscard]] const char* ToString(ImageError error);

// Decodes an encoded image file held in memory to RGBA8.
[[nodiscard]] core::Result<ImageData, ImageError> DecodeImage(
    std::span<const std::byte> bytes);

// Reads and decodes the image file at `path` to RGBA8.
[[nodiscard]] core::Result<ImageData, ImageError> LoadImageFile(
    const std::string& path);

// Reads and decodes the image file at `path` to linear float RGB. Meant for
// Radiance .hdr; 8-bit formats are linearized with stb_image's default
// gamma 2.2 (an approximation of sRGB).
[[nodiscard]] core::Result<ImageDataF, ImageError> LoadHdrFile(
    const std::string& path);

// Packs separate greyscale roughness and metallic maps (R channel each) into
// one glTF metallic-roughness image: G = roughness, B = metallic, R = 0,
// A = 255. A null input is treated as all 255, so the material's scalar
// factor applies unchanged. When the sizes differ, the smaller image is
// resampled (bilinear) to the larger width and height. Returns an empty
// ImageData if both inputs are null.
[[nodiscard]] ImageData PackMetallicRoughness(const ImageData* roughness,
                                              const ImageData* metallic);

}  // namespace blue::asset

#endif  // BLUE_ASSET_IMAGE_DECODER_H_
