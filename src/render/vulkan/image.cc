#include "render/vulkan/image.h"

#include <algorithm>
#include <bit>
#include <utility>
#include <vector>

#include "core/log.h"
#include "render/vulkan/barriers.h"
#include "render/vulkan/staging_upload.h"
#include "render/vulkan/vk_check.h"

namespace blue::render::vulkan {

namespace {

constexpr uint32_t kCubeFaces = 6;

VkImageViewType FullViewType(const ImageDesc& desc) {
  if (desc.cube) {
    return VK_IMAGE_VIEW_TYPE_CUBE;
  }
  return desc.array_layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY
                               : VK_IMAGE_VIEW_TYPE_2D;
}

// Returns the extent of mip `level` of `extent`; never below 1 x 1.
VkExtent2D MipExtent(VkExtent2D extent, uint32_t level) {
  return {std::max(extent.width >> level, 1u),
          std::max(extent.height >> level, 1u)};
}

// Blit offset of the far corner of `extent`.
VkOffset3D FarCorner(VkExtent2D extent) {
  return {static_cast<int32_t>(extent.width),
          static_cast<int32_t>(extent.height), 1};
}

// Records the blits that fill mips 1.. of `image` from mip 0, which must be
// in TRANSFER_DST_OPTIMAL like every other mip. Leaves all mips in
// SHADER_READ_ONLY_OPTIMAL.
void RecordMipChain(VkCommandBuffer cmd, const Image& image, VkFilter filter) {
  const VkImage handle = image.handle();
  const uint32_t layers = image.array_layers();
  constexpr BarrierScope kTransferWrite{VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                        VK_ACCESS_2_TRANSFER_WRITE_BIT};
  constexpr BarrierScope kTransferRead{VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                       VK_ACCESS_2_TRANSFER_READ_BIT};

  for (uint32_t level = 1; level < image.mip_levels(); ++level) {
    const VkImageSubresourceRange source{VK_IMAGE_ASPECT_COLOR_BIT, level - 1,
                                         1, 0, layers};
    ImageBarrier(cmd, handle, source, kTransferWrite, kTransferRead,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    const VkImageBlit2 region{
        .sType = VK_STRUCTURE_TYPE_IMAGE_BLIT_2,
        .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, layers},
        .srcOffsets = {{0, 0, 0},
                       FarCorner(MipExtent(image.extent(), level - 1))},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, layers},
        .dstOffsets = {{0, 0, 0}, FarCorner(MipExtent(image.extent(), level))},
    };
    const VkBlitImageInfo2 blit{
        .sType = VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2,
        .srcImage = handle,
        .srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .dstImage = handle,
        .dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .regionCount = 1,
        .pRegions = &region,
        .filter = filter,
    };
    vkCmdBlitImage2(cmd, &blit);
    ImageBarrier(cmd, handle, source, kTransferRead, kFragmentSampledRead,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  }
  const VkImageSubresourceRange last{VK_IMAGE_ASPECT_COLOR_BIT,
                                     image.mip_levels() - 1, 1, 0, layers};
  ImageBarrier(cmd, handle, last, kTransferWrite, kFragmentSampledRead,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

}  // namespace

uint32_t MipLevelCount(VkExtent2D extent) {
  const uint32_t largest = std::max({extent.width, extent.height, 1u});
  return static_cast<uint32_t>(std::bit_width(largest));
}

uint32_t TexelSize(VkFormat format) {
  switch (format) {
    case VK_FORMAT_R8_UNORM:
      return 1;
    case VK_FORMAT_R8G8_UNORM:
      return 2;
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_R16G16_SFLOAT:
    case VK_FORMAT_R32_SFLOAT:
      return 4;
    case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_R32G32_SFLOAT:
      return 8;
    case VK_FORMAT_R32G32B32A32_SFLOAT:
      return 16;
    default:
      return 0;
  }
}

ImageView::~ImageView() { Release(); }

ImageView::ImageView(ImageView&& other) noexcept
    : device_(std::exchange(other.device_, VK_NULL_HANDLE)),
      view_(std::exchange(other.view_, VK_NULL_HANDLE)) {}

ImageView& ImageView::operator=(ImageView&& other) noexcept {
  if (this != &other) {
    Release();
    device_ = std::exchange(other.device_, VK_NULL_HANDLE);
    view_ = std::exchange(other.view_, VK_NULL_HANDLE);
  }
  return *this;
}

void ImageView::Release() {
  if (view_ != VK_NULL_HANDLE) {
    vkDestroyImageView(device_, view_, nullptr);
    view_ = VK_NULL_HANDLE;
  }
}

Image::Image(const VulkanContext& context, const ImageDesc& desc)
    : device_(context.device()),
      allocator_(context.allocator()),
      format_(desc.format),
      extent_(desc.extent),
      aspect_(desc.aspect),
      array_layers_(desc.array_layers),
      mip_levels_(std::max(desc.mip_levels, 1u)) {
  if (desc.cube && (desc.array_layers != kCubeFaces ||
                    desc.extent.width != desc.extent.height)) {
    core::log::Fatal("A cube image needs 6 layers and a square extent");
  }
  const VkImageCreateInfo image_info{
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .flags = desc.cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT
                         : VkImageCreateFlags{0},
      .imageType = VK_IMAGE_TYPE_2D,
      .format = desc.format,
      .extent = {desc.extent.width, desc.extent.height, 1},
      .mipLevels = mip_levels_,
      .arrayLayers = desc.array_layers,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = desc.usage,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
  };
  const VmaAllocationCreateInfo alloc_info{
      .flags = desc.dedicated ? VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT
                              : VmaAllocationCreateFlags{0},
      .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
  };
  VmaAllocationInfo allocation_info{};
  CheckVk(vmaCreateImage(allocator_, &image_info, &alloc_info, &image_,
                         &allocation_, &allocation_info));
  size_bytes_ = allocation_info.size;

  view_ = CreateRawView(
      {.type = FullViewType(desc), .mip_count = 0, .layer_count = 0});
  if (desc.array_layers > 1 && !desc.cube) {
    layer_views_.resize(desc.array_layers);
    for (uint32_t layer = 0; layer < desc.array_layers; ++layer) {
      layer_views_[layer] = CreateRawView({.type = VK_IMAGE_VIEW_TYPE_2D,
                                           .base_layer = layer,
                                           .layer_count = 1});
    }
  }
}

ImageView Image::CreateView(const ImageViewDesc& desc) const {
  return ImageView(device_, CreateRawView(desc));
}

VkImageView Image::CreateRawView(const ImageViewDesc& desc) const {
  const VkImageViewCreateInfo view_info{
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = image_,
      .viewType = desc.type,
      .format = format_,
      .subresourceRange = {aspect_, desc.base_mip,
                           desc.mip_count == 0 ? VK_REMAINING_MIP_LEVELS
                                               : desc.mip_count,
                           desc.base_layer,
                           desc.layer_count == 0 ? VK_REMAINING_ARRAY_LAYERS
                                                 : desc.layer_count},
  };
  VkImageView view = VK_NULL_HANDLE;
  CheckVk(vkCreateImageView(device_, &view_info, nullptr, &view));
  return view;
}

Image::~Image() { Release(); }

Image::Image(Image&& other) noexcept
    : device_(std::exchange(other.device_, VK_NULL_HANDLE)),
      allocator_(std::exchange(other.allocator_, VK_NULL_HANDLE)),
      image_(std::exchange(other.image_, VK_NULL_HANDLE)),
      view_(std::exchange(other.view_, VK_NULL_HANDLE)),
      layer_views_(std::move(other.layer_views_)),
      allocation_(std::exchange(other.allocation_, VK_NULL_HANDLE)),
      format_(std::exchange(other.format_, VK_FORMAT_UNDEFINED)),
      extent_(std::exchange(other.extent_, {})),
      aspect_(std::exchange(other.aspect_, VK_IMAGE_ASPECT_COLOR_BIT)),
      array_layers_(std::exchange(other.array_layers_, 1)),
      mip_levels_(std::exchange(other.mip_levels_, 1)),
      size_bytes_(std::exchange(other.size_bytes_, 0)) {
  other.layer_views_.clear();
}

Image& Image::operator=(Image&& other) noexcept {
  if (this != &other) {
    Release();
    device_ = std::exchange(other.device_, VK_NULL_HANDLE);
    allocator_ = std::exchange(other.allocator_, VK_NULL_HANDLE);
    image_ = std::exchange(other.image_, VK_NULL_HANDLE);
    view_ = std::exchange(other.view_, VK_NULL_HANDLE);
    layer_views_ = std::move(other.layer_views_);
    other.layer_views_.clear();
    allocation_ = std::exchange(other.allocation_, VK_NULL_HANDLE);
    format_ = std::exchange(other.format_, VK_FORMAT_UNDEFINED);
    extent_ = std::exchange(other.extent_, {});
    aspect_ = std::exchange(other.aspect_, VK_IMAGE_ASPECT_COLOR_BIT);
    array_layers_ = std::exchange(other.array_layers_, 1);
    mip_levels_ = std::exchange(other.mip_levels_, 1);
    size_bytes_ = std::exchange(other.size_bytes_, 0);
  }
  return *this;
}

void Image::Release() {
  if (image_ != VK_NULL_HANDLE) {
    for (const VkImageView layer_view : layer_views_) {
      vkDestroyImageView(device_, layer_view, nullptr);
    }
    layer_views_.clear();
    vkDestroyImageView(device_, view_, nullptr);
    vmaDestroyImage(allocator_, image_, allocation_);
    image_ = VK_NULL_HANDLE;
    view_ = VK_NULL_HANDLE;
    allocation_ = VK_NULL_HANDLE;
  }
}

Image CreateSampledImage(const VulkanContext& context, ImageDesc desc,
                         std::span<const std::byte> texels) {
  const VkDeviceSize expected = VkDeviceSize{TexelSize(desc.format)} *
                                desc.extent.width * desc.extent.height *
                                desc.array_layers;
  if (expected == 0 || texels.size_bytes() != expected) {
    core::log::Fatal("Texture upload of {} bytes, expected {} (format {})",
                     texels.size_bytes(), expected,
                     static_cast<int>(desc.format));
  }

  VkFilter mip_filter = VK_FILTER_LINEAR;
  if (desc.mip_levels > 1) {
    VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(context.physical_device(), desc.format,
                                        &properties);
    const VkFormatFeatureFlags features = properties.optimalTilingFeatures;
    constexpr VkFormatFeatureFlags kBlit =
        VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
    if ((features & kBlit) != kBlit) {
      core::log::Warn("Format {} cannot be blitted: texture gets no mips",
                      static_cast<int>(desc.format));
      desc.mip_levels = 1;
    } else if ((features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) ==
               0) {
      mip_filter = VK_FILTER_NEAREST;
    }
  }
  desc.usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  if (desc.mip_levels > 1) {
    desc.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  }
  Image image(context, desc);

  StagingUploader uploader(context, expected);
  ImageBarrier(
      uploader.cmd(), image.handle(), VK_IMAGE_ASPECT_COLOR_BIT, kNoAccess,
      {VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT},
      VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  const size_t layer_bytes = texels.size_bytes() / desc.array_layers;
  for (uint32_t layer = 0; layer < desc.array_layers; ++layer) {
    uploader.CopyToImage(texels.subspan(layer * layer_bytes, layer_bytes),
                         image.handle(), desc.format, desc.extent, 0, layer);
  }
  RecordMipChain(uploader.cmd(), image, mip_filter);
  uploader.Finish();
  return image;
}

std::vector<Image> CreateImagesFromMipChains(
    const VulkanContext& context, std::span<const MipChainUpload> uploads) {
  VkDeviceSize total_bytes = 0;
  for (const MipChainUpload& upload : uploads) {
    if (upload.mips.size() != upload.desc.mip_levels ||
        upload.desc.array_layers != 1) {
      core::log::Fatal("Mip chain upload: {} levels given for {}",
                       upload.mips.size(), upload.desc.mip_levels);
    }
    for (const std::span<const std::byte> level : upload.mips) {
      total_bytes += level.size_bytes();
    }
  }
  std::vector<Image> images;
  images.reserve(uploads.size());
  if (uploads.empty()) {
    return images;
  }

  // Image after image: its barrier into TRANSFER_DST, its levels (split
  // over staging chunks as they fill up), its barrier to sampled reads.
  constexpr BarrierScope kTransferWrite{VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                        VK_ACCESS_2_TRANSFER_WRITE_BIT};
  StagingUploader uploader(context, total_bytes);
  for (const MipChainUpload& upload : uploads) {
    ImageDesc desc = upload.desc;
    desc.usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    const Image& image = images.emplace_back(context, desc);
    ImageBarrier(uploader.cmd(), image.handle(), VK_IMAGE_ASPECT_COLOR_BIT,
                 kNoAccess, kTransferWrite, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    for (uint32_t level = 0; level < desc.mip_levels; ++level) {
      uploader.CopyToImage(upload.mips[level], image.handle(), desc.format,
                           desc.extent, level, 0);
    }
    ImageBarrier(uploader.cmd(), image.handle(), VK_IMAGE_ASPECT_COLOR_BIT,
                 kTransferWrite, kFragmentSampledRead,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  }
  uploader.Finish();
  return images;
}

}  // namespace blue::render::vulkan
