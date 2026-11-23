#include "render/d3d12/texture_cache.h"

#include <d3d12.h>

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
#include "render/d3d12/d3d12_context.h"
#include "render/d3d12/format.h"
#include "render/d3d12/image.h"
#include "render/d3d12/staging_upload.h"
#include "render/texture_types.h"

namespace blue::render::d3d12 {

namespace {

// One texture to create and fill.
struct MipChain {
  TextureFormat format = TextureFormat::kRgba8Unorm;
  core::Extent2D extent;
  std::span<const std::span<const std::byte>> mips;
  const wchar_t* name = L"blue texture";
};

// Creates the textures of `chains` and uploads their mips in one batch;
// returns them in PIXEL_SHADER_RESOURCE | NON_PIXEL_SHADER_RESOURCE.
std::vector<Image> CreateTextures(const D3d12Context& context,
                                  std::span<const MipChain> chains) {
  uint64_t total_bytes = 0;
  for (const MipChain& chain : chains) {
    for (uint32_t level = 0; level < chain.mips.size(); ++level) {
      total_bytes += AlignUp(
          MipFootprintOf(chain.format, chain.extent, level).upload_bytes,
          kPlacementAlignment);
    }
  }
  std::vector<Image> images;
  images.reserve(chains.size());
  StagingUploader uploader(context, total_bytes);
  for (const MipChain& chain : chains) {
    const DXGI_FORMAT format = ToDxgiFormat(chain.format);
    const auto levels = static_cast<uint32_t>(chain.mips.size());
    Image& image = images.emplace_back(
        context, ImageDesc{.extent = chain.extent,
                           .mip_levels = levels,
                           .format = format,
                           .initial_state = D3D12_RESOURCE_STATE_COPY_DEST,
                           .name = chain.name});
    image.CreateSrv(format, /*shader_visible=*/true);
    for (uint32_t level = 0; level < levels; ++level) {
      uploader.CopyToTexture(chain.mips[level], image.resource(), format,
                             CopyBlockOf(chain.format), chain.extent, level, 0,
                             levels);
    }
  }
  {
    BarrierBatch barriers(uploader.cmd());
    for (Image& image : images) {
      image.Transition(barriers, kSampledState);
    }
  }
  uploader.Finish();
  return images;
}

Image MakeSolidTexture(const D3d12Context& context, std::array<uint8_t, 4> rgba,
                       const wchar_t* name) {
  const std::array<std::span<const std::byte>, 1> mips = {
      std::as_bytes(std::span(rgba))};
  const std::array<MipChain, 1> chain = {{{.format = TextureFormat::kRgba8Unorm,
                                           .extent = {1, 1},
                                           .mips = mips,
                                           .name = name}}};
  return std::move(CreateTextures(context, chain).front());
}

}  // namespace

Image CreateSampledImage(
    const D3d12Context& context, const ImageDesc& desc, CopyBlock block,
    std::span<const std::span<const std::byte>> subresources) {
  if (subresources.size() != size_t{desc.mip_levels} * desc.layers) {
    core::log::Fatal("Texture of {} mips x {} layers given {} subresources",
                     desc.mip_levels, desc.layers, subresources.size());
  }
  ImageDesc sampled = desc;
  sampled.flags = D3D12_RESOURCE_FLAG_NONE;
  sampled.initial_state = D3D12_RESOURCE_STATE_COPY_DEST;
  sampled.clear_value.reset();
  Image image(context, sampled);
  image.CreateSrv(desc.format, /*shader_visible=*/false);
  uint64_t total_bytes = 0;
  for (const std::span<const std::byte> bytes : subresources) {
    total_bytes += AlignUp(bytes.size_bytes(), kPlacementAlignment) * 2;
  }
  StagingUploader uploader(context, total_bytes);
  for (uint32_t layer = 0; layer < desc.layers; ++layer) {
    for (uint32_t mip = 0; mip < desc.mip_levels; ++mip) {
      uploader.CopyToTexture(subresources[image.subresource(mip, layer)],
                             image.resource(), desc.format, block, desc.extent,
                             mip, layer, desc.mip_levels);
    }
  }
  {
    BarrierBatch barriers(uploader.cmd());
    image.Transition(barriers, kSampledState);
  }
  uploader.Finish();
  return image;
}

TextureCache::TextureCache(const D3d12Context& context) : context_(context) {
  defaults_[static_cast<size_t>(DefaultTexture::kWhite)] =
      MakeSolidTexture(context_, {255, 255, 255, 255}, L"blue default white");
  defaults_[static_cast<size_t>(DefaultTexture::kFlatNormal)] =
      MakeSolidTexture(context_, {128, 128, 255, 255},
                       L"blue default flat normal");
  defaults_[static_cast<size_t>(DefaultTexture::kBlack)] =
      MakeSolidTexture(context_, {0, 0, 0, 255}, L"blue default black");
}

Image TextureCache::TakeUploaded(TextureId id) {
  const auto index = static_cast<size_t>(id);
  if (index >= uploaded_.size() || !uploaded_[index].valid()) {
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
  std::vector<MipChain> chains;
  chains.reserve(textures.size());
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
    chains.push_back({.format = texture.format,
                      .extent = extent,
                      .mips = texture.mips,
                      .name = L"blue uploaded texture"});
  }

  std::vector<Image> images = CreateTextures(context_, chains);
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

}  // namespace blue::render::d3d12
