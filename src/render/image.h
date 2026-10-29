#ifndef BLUE_RENDER_IMAGE_H_
#define BLUE_RENDER_IMAGE_H_

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include "render/vulkan_context.h"

namespace blue::render {

struct ImageDesc {
  VkExtent2D extent{};
  VkFormat format = VK_FORMAT_UNDEFINED;
  VkImageUsageFlags usage = 0;
  VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
};

// Device-local 2D image with a matching view, backed by a VMA allocation.
// Move-only; a moved-from image is empty. Not thread-safe.
class Image {
 public:
  Image() = default;
  Image(const VulkanContext& context, const ImageDesc& desc);
  ~Image();

  Image(const Image&) = delete;
  Image& operator=(const Image&) = delete;
  Image(Image&& other) noexcept;
  Image& operator=(Image&& other) noexcept;

  [[nodiscard]] VkImage handle() const { return image_; }
  [[nodiscard]] VkImageView view() const { return view_; }
  [[nodiscard]] VkFormat format() const { return format_; }

 private:
  void Release();

  VkDevice device_ = VK_NULL_HANDLE;
  VmaAllocator allocator_ = VK_NULL_HANDLE;
  VkImage image_ = VK_NULL_HANDLE;
  VkImageView view_ = VK_NULL_HANDLE;
  VmaAllocation allocation_ = VK_NULL_HANDLE;
  VkFormat format_ = VK_FORMAT_UNDEFINED;
};

}  // namespace blue::render

#endif  // BLUE_RENDER_IMAGE_H_
