#include "render/texture_cache.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "core/extent.h"
#include "core/log.h"
#include "render/vk_extent.h"

namespace blue::render {

namespace {

Image MakeSolidTexture(const VulkanContext& context,
                       std::array<uint8_t, 4> rgba) {
  return CreateSampledImage(
      context, {.extent = {1, 1}, .format = VK_FORMAT_R8G8B8A8_UNORM},
      std::as_bytes(std::span(rgba)));
}

}  // namespace

TextureCache::TextureCache(const VulkanContext& context) : context_(context) {
  defaults_[static_cast<size_t>(DefaultTexture::kWhite)] =
      MakeSolidTexture(context_, {255, 255, 255, 255});
  defaults_[static_cast<size_t>(DefaultTexture::kFlatNormal)] =
      MakeSolidTexture(context_, {128, 128, 255, 255});
  defaults_[static_cast<size_t>(DefaultTexture::kBlack)] =
      MakeSolidTexture(context_, {0, 0, 0, 255});
}

VkFormat ToVkFormat(TextureFormat format) {
  switch (format) {
    case TextureFormat::kBc7Srgb:
      return VK_FORMAT_BC7_SRGB_BLOCK;
    case TextureFormat::kBc7Unorm:
      return VK_FORMAT_BC7_UNORM_BLOCK;
    case TextureFormat::kBc5Unorm:
      return VK_FORMAT_BC5_UNORM_BLOCK;
    case TextureFormat::kRgba8Srgb:
      return VK_FORMAT_R8G8B8A8_SRGB;
    case TextureFormat::kRgba8Unorm:
      return VK_FORMAT_R8G8B8A8_UNORM;
  }
  core::log::Fatal("Unknown texture format {}", static_cast<int>(format));
}

Image TextureCache::TakeUploaded(TextureId id) {
  const auto index = static_cast<size_t>(id);
  if (index >= uploaded_.size() ||
      uploaded_[index].handle() == VK_NULL_HANDLE) {
    return {};
  }
  Image taken = std::move(uploaded_[index]);
  uploaded_[index] = Image();
  stats_.count -= 1;
  stats_.bytes -= taken.size_bytes();
  return taken;
}

std::vector<TextureId> TextureCache::Upload(
    std::span<const TextureUpload> textures) {
  const auto start = std::chrono::steady_clock::now();
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
    uploads.push_back({.desc = {.extent = ToVkExtent(extent),
                                .format = ToVkFormat(texture.format),
                                .mip_levels = static_cast<uint32_t>(levels)},
                       .mips = texture.mips});
  }

  std::vector<Image> images = CreateImagesFromMipChains(context_, uploads);
  std::vector<TextureId> ids;
  ids.reserve(images.size());
  VkDeviceSize device_bytes = 0;
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
