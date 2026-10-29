#include "render/image.h"

#include <utility>

#include "render/vk_check.h"

namespace blue::render {

Image::Image(const VulkanContext& context, const ImageDesc& desc)
    : device_(context.device()),
      allocator_(context.allocator()),
      format_(desc.format) {
  const VkImageCreateInfo image_info{
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = desc.format,
      .extent = {desc.extent.width, desc.extent.height, 1},
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = desc.usage,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
  };
  const VmaAllocationCreateInfo alloc_info{
      // Render targets benefit from dedicated memory.
      .flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT,
      .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
  };
  CheckVk(vmaCreateImage(allocator_, &image_info, &alloc_info, &image_,
                         &allocation_, nullptr));

  const VkImageViewCreateInfo view_info{
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = image_,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = desc.format,
      .subresourceRange = {desc.aspect, 0, 1, 0, 1},
  };
  CheckVk(vkCreateImageView(device_, &view_info, nullptr, &view_));
}

Image::~Image() { Release(); }

Image::Image(Image&& other) noexcept
    : device_(std::exchange(other.device_, VK_NULL_HANDLE)),
      allocator_(std::exchange(other.allocator_, VK_NULL_HANDLE)),
      image_(std::exchange(other.image_, VK_NULL_HANDLE)),
      view_(std::exchange(other.view_, VK_NULL_HANDLE)),
      allocation_(std::exchange(other.allocation_, VK_NULL_HANDLE)),
      format_(std::exchange(other.format_, VK_FORMAT_UNDEFINED)) {}

Image& Image::operator=(Image&& other) noexcept {
  if (this != &other) {
    Release();
    device_ = std::exchange(other.device_, VK_NULL_HANDLE);
    allocator_ = std::exchange(other.allocator_, VK_NULL_HANDLE);
    image_ = std::exchange(other.image_, VK_NULL_HANDLE);
    view_ = std::exchange(other.view_, VK_NULL_HANDLE);
    allocation_ = std::exchange(other.allocation_, VK_NULL_HANDLE);
    format_ = std::exchange(other.format_, VK_FORMAT_UNDEFINED);
  }
  return *this;
}

void Image::Release() {
  if (image_ != VK_NULL_HANDLE) {
    vkDestroyImageView(device_, view_, nullptr);
    vmaDestroyImage(allocator_, image_, allocation_);
    image_ = VK_NULL_HANDLE;
    view_ = VK_NULL_HANDLE;
    allocation_ = VK_NULL_HANDLE;
  }
}

}  // namespace blue::render
