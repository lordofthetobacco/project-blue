#include "asset/image_decoder.h"

#include <algorithm>
#include <climits>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "core/log.h"
#include "platform/file_system.h"

// The one translation unit that compiles stb_image. Files are read through
// platform::ReadFile (UTF-8 paths on Windows), so stb's stdio layer is off.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_TGA
#define STBI_ONLY_HDR
#include <stb_image.h>
#undef STBI_ONLY_HDR
#undef STBI_ONLY_TGA
#undef STBI_ONLY_BMP
#undef STBI_ONLY_JPEG
#undef STBI_ONLY_PNG
#undef STBI_NO_STDIO
#undef STB_IMAGE_IMPLEMENTATION

namespace blue::asset {
namespace {

// Largest image file read from disk.
constexpr size_t kMaxImageFileSize = size_t{512} * 1024 * 1024;

using ImageResult = core::Result<ImageData, ImageError>;

// Frees stb_image allocations.
struct StbiDeleter {
  void operator()(void* pixels) const { stbi_image_free(pixels); }
};

// Returns why `bytes` cannot be decoded before decoding it: kDecodeFailed if
// stb_image does not recognise it, kTooLarge if a dimension exceeds
// kMaxImageDimension. nullopt means "go ahead".
std::optional<ImageError> CheckHeader(std::span<const std::byte> bytes) {
  if (bytes.empty() || bytes.size() > static_cast<size_t>(INT_MAX)) {
    return ImageError::kDecodeFailed;
  }
  int width = 0;
  int height = 0;
  int channels = 0;
  if (stbi_info_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()),
                            static_cast<int>(bytes.size()), &width, &height,
                            &channels) == 0) {
    return ImageError::kDecodeFailed;
  }
  if (width <= 0 || height <= 0 || width > kMaxImageDimension ||
      height > kMaxImageDimension) {
    return ImageError::kTooLarge;
  }
  return std::nullopt;
}

// Returns the red channel of `image` at normalized coordinate (x, y) with
// bilinear filtering and clamping.
float SampleRed(const ImageData& image, float x, float y) {
  const float fx = std::clamp(x * static_cast<float>(image.width) - 0.5f, 0.0f,
                              static_cast<float>(image.width - 1));
  const float fy = std::clamp(y * static_cast<float>(image.height) - 0.5f, 0.0f,
                              static_cast<float>(image.height - 1));
  const int x0 = static_cast<int>(fx);
  const int y0 = static_cast<int>(fy);
  const int x1 = std::min(x0 + 1, image.width - 1);
  const int y1 = std::min(y0 + 1, image.height - 1);
  const float tx = fx - static_cast<float>(x0);
  const float ty = fy - static_cast<float>(y0);
  const auto red = [&](int px, int py) {
    return static_cast<float>(
        image.pixels[(static_cast<size_t>(py) *
                          static_cast<size_t>(image.width) +
                      static_cast<size_t>(px)) *
                     4]);
  };
  const float top = red(x0, y0) + (red(x1, y0) - red(x0, y0)) * tx;
  const float bottom = red(x0, y1) + (red(x1, y1) - red(x0, y1)) * tx;
  return top + (bottom - top) * ty;
}

// Returns true if `image` has pixels matching its size.
bool IsUsable(const ImageData* image) {
  return image != nullptr && image->width > 0 && image->height > 0 &&
         image->pixels.size() == static_cast<size_t>(image->width) *
                                     static_cast<size_t>(image->height) * 4;
}

}  // namespace

const char* ToString(ImageError error) {
  switch (error) {
    case ImageError::kFileNotFound:
      return "file not found";
    case ImageError::kDecodeFailed:
      return "decode failed";
    case ImageError::kTooLarge:
      return "image too large";
    case ImageError::kNoTexture:
      return "no such texture";
  }
  return "unknown";
}

core::Result<ImageData, ImageError> DecodeImage(
    std::span<const std::byte> bytes) {
  if (const std::optional<ImageError> error = CheckHeader(bytes)) {
    return ImageResult::Error(*error);
  }
  int width = 0;
  int height = 0;
  int source_channels = 0;
  const std::unique_ptr<stbi_uc, StbiDeleter> pixels(stbi_load_from_memory(
      reinterpret_cast<const stbi_uc*>(bytes.data()),
      static_cast<int>(bytes.size()), &width, &height, &source_channels, 4));
  if (pixels == nullptr) {
    return ImageResult::Error(ImageError::kDecodeFailed);
  }
  ImageData image{.width = width, .height = height};
  image.pixels.assign(pixels.get(),
                      pixels.get() + static_cast<size_t>(width) *
                                         static_cast<size_t>(height) * 4);
  return ImageResult::Ok(std::move(image));
}

core::Result<ImageData, ImageError> LoadImageFile(const std::string& path) {
  const auto bytes = platform::ReadFile(path, kMaxImageFileSize);
  if (!bytes.ok()) {
    return ImageResult::Error(ImageError::kFileNotFound);
  }
  return DecodeImage(bytes.value());
}

core::Result<ImageDataF, ImageError> LoadHdrFile(const std::string& path) {
  using Result = core::Result<ImageDataF, ImageError>;
  const auto bytes = platform::ReadFile(path, kMaxImageFileSize);
  if (!bytes.ok()) {
    return Result::Error(ImageError::kFileNotFound);
  }
  if (const std::optional<ImageError> error = CheckHeader(bytes.value())) {
    return Result::Error(*error);
  }
  int width = 0;
  int height = 0;
  int source_channels = 0;
  const std::unique_ptr<float, StbiDeleter> pixels(stbi_loadf_from_memory(
      reinterpret_cast<const stbi_uc*>(bytes.value().data()),
      static_cast<int>(bytes.value().size()), &width, &height, &source_channels,
      3));
  if (pixels == nullptr) {
    return Result::Error(ImageError::kDecodeFailed);
  }
  ImageDataF image{.width = width, .height = height};
  image.pixels.assign(pixels.get(),
                      pixels.get() + static_cast<size_t>(width) *
                                         static_cast<size_t>(height) * 3);
  return Result::Ok(std::move(image));
}

ImageData PackMetallicRoughness(const ImageData* roughness,
                                const ImageData* metallic) {
  const bool has_roughness = IsUsable(roughness);
  const bool has_metallic = IsUsable(metallic);
  if (!has_roughness && !has_metallic) {
    return {};
  }
  ImageData packed{.width = std::max(has_roughness ? roughness->width : 0,
                                     has_metallic ? metallic->width : 0),
                   .height = std::max(has_roughness ? roughness->height : 0,
                                      has_metallic ? metallic->height : 0)};
  packed.pixels.resize(static_cast<size_t>(packed.width) *
                       static_cast<size_t>(packed.height) * 4);

  // Returns the red channel of `image` at packed texel (x, y): a direct read
  // when the sizes match, bilinear otherwise, 255 for a missing image.
  const auto channel = [&packed](const ImageData* image, bool usable, int x,
                                 int y) -> uint8_t {
    if (!usable) {
      return 255;
    }
    if (image->width == packed.width && image->height == packed.height) {
      return image
          ->pixels[(static_cast<size_t>(y) * static_cast<size_t>(packed.width) +
                    static_cast<size_t>(x)) *
                   4];
    }
    const float value = SampleRed(
        *image,
        (static_cast<float>(x) + 0.5f) / static_cast<float>(packed.width),
        (static_cast<float>(y) + 0.5f) / static_cast<float>(packed.height));
    return static_cast<uint8_t>(std::clamp(value + 0.5f, 0.0f, 255.0f));
  };
  for (int y = 0; y < packed.height; ++y) {
    for (int x = 0; x < packed.width; ++x) {
      uint8_t* texel = &packed.pixels[(static_cast<size_t>(y) *
                                           static_cast<size_t>(packed.width) +
                                       static_cast<size_t>(x)) *
                                      4];
      texel[0] = 0;
      texel[1] = channel(roughness, has_roughness, x, y);
      texel[2] = channel(metallic, has_metallic, x, y);
      texel[3] = 255;
    }
  }
  return packed;
}

}  // namespace blue::asset
