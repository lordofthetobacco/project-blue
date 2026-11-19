#include "render/texture_cache.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "core/extent.h"
#include "core/log.h"

namespace blue::render {

namespace {

Image MakeSolidTexture(const MetalContext& context, std::array<uint8_t, 4> rgba,
                       const char* label) {
  return CreateSampledImage(context,
                            {.extent = {1, 1},
                             .format = MTLPixelFormatRGBA8Unorm,
                             .label = label},
                            std::as_bytes(std::span(rgba)));
}

}  // namespace

TextureCache::TextureCache(const MetalContext& context) : context_(context) {
  defaults_[static_cast<size_t>(DefaultTexture::kWhite)] =
      MakeSolidTexture(context_, {255, 255, 255, 255}, "Default white");
  defaults_[static_cast<size_t>(DefaultTexture::kFlatNormal)] =
      MakeSolidTexture(context_, {128, 128, 255, 255}, "Default flat normal");
  defaults_[static_cast<size_t>(DefaultTexture::kBlack)] =
      MakeSolidTexture(context_, {0, 0, 0, 255}, "Default black");
}

MTLPixelFormat ToPixelFormat(TextureFormat format) {
  switch (format) {
    case TextureFormat::kBc7Srgb:
      return MTLPixelFormatBC7_RGBAUnorm_sRGB;
    case TextureFormat::kBc7Unorm:
      return MTLPixelFormatBC7_RGBAUnorm;
    case TextureFormat::kBc5Unorm:
      return MTLPixelFormatBC5_RGUnorm;
    case TextureFormat::kRgba8Srgb:
      return MTLPixelFormatRGBA8Unorm_sRGB;
    case TextureFormat::kRgba8Unorm:
      return MTLPixelFormatRGBA8Unorm;
  }
  core::log::Fatal("Unknown texture format {}", static_cast<int>(format));
}

Image TextureCache::TakeUploaded(TextureId id) {
  const auto index = static_cast<size_t>(id);
  if (index >= uploaded_.size() || uploaded_[index].texture() == nil) {
    return {};
  }
  // The defaulted move leaves an empty Image in the slot: the tombstone.
  Image taken = std::move(uploaded_[index]);
  stats_.count -= 1;
  stats_.bytes -= taken.size_bytes();
  return taken;
}

std::vector<TextureId> TextureCache::Upload(
    std::span<const TextureUpload> textures) {
  const auto start = std::chrono::steady_clock::now();
  // NUL-terminated copies of the names, for the texture labels; alive until
  // the images are created. Reserved up front so that no reallocation moves
  // the strings that `uploads` points into.
  std::vector<std::string> labels;
  labels.reserve(textures.size());
  std::vector<MipChainUpload> uploads;
  uploads.reserve(textures.size());
  size_t file_bytes = 0;
  for (const TextureUpload& texture : textures) {
    const core::Extent2D extent{texture.width, texture.height};
    const size_t levels = texture.mips.size();
    if (texture.width == 0 || texture.height == 0 || levels == 0 ||
        levels > MipLevelCount(extent)) {
      core::log::Fatal("Texture '{}': {} x {} with {} mips cannot be uploaded",
                       texture.name, texture.width, texture.height, levels);
    }
    for (uint32_t level = 0; level < levels; ++level) {
      const uint32_t width = std::max(texture.width >> level, 1u);
      const uint32_t height = std::max(texture.height >> level, 1u);
      const size_t expected = MipByteSize(texture.format, width, height);
      if (texture.mips[level].size_bytes() != expected) {
        core::log::Fatal("Texture '{}' mip {}: {} bytes, expected {}",
                         texture.name, level, texture.mips[level].size_bytes(),
                         expected);
      }
      file_bytes += expected;
    }
    const std::string& label = labels.emplace_back(
        texture.name.empty() ? std::string("Uploaded texture")
                             : std::string(texture.name));
    uploads.push_back({.desc = {.extent = extent,
                                .format = ToPixelFormat(texture.format),
                                .mip_levels = static_cast<uint32_t>(levels),
                                .label = label.c_str()},
                       .mips = texture.mips});
  }

  std::vector<Image> images = CreateImagesFromMipChains(context_, uploads);
  std::vector<TextureId> ids;
  ids.reserve(images.size());
  uint64_t device_bytes = 0;
  for (Image& image : images) {
    device_bytes += image.size_bytes();
    ids.push_back(static_cast<TextureId>(uploaded_.size()));
    uploaded_.push_back(std::move(image));
  }
  stats_.count += images.size();
  stats_.bytes += device_bytes;

  using Milliseconds = std::chrono::duration<double, std::milli>;
  const double elapsed =
      Milliseconds(std::chrono::steady_clock::now() - start).count();
  core::log::Info(
      "Uploaded {} encoded textures ({:.1f} MiB data, {:.1f} MiB device) in "
      "{:.1f} ms",
      ids.size(), static_cast<double>(file_bytes) / (1024.0 * 1024.0),
      static_cast<double>(device_bytes) / (1024.0 * 1024.0), elapsed);
  return ids;
}

}  // namespace blue::render
