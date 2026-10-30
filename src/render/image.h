#ifndef BLUE_RENDER_IMAGE_H_
#define BLUE_RENDER_IMAGE_H_

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

#include "render/vulkan_context.h"

namespace blue::render {

struct ImageDesc {
  VkExtent2D extent{};
  VkFormat format = VK_FORMAT_UNDEFINED;
  VkImageUsageFlags usage = 0;
  VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
  // More than one layer makes an array image.
  uint32_t array_layers = 1;
};

// Device-local 2D image with a matching view, backed by a VMA allocation. An
// image with several layers also has one 2D view per layer. Move-only; a
// moved-from image is empty. Not thread-safe.
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
  // Returns a 2D view of a single-layer image, or a 2D-array view covering all
  // layers of an array image.
  [[nodiscard]] VkImageView view() const { return view_; }
  // Returns the 2D view of `layer`, which must be less than array_layers() of
  // an array image. Only array images have these views; a single-layer image
  // returns view().
  [[nodiscard]] VkImageView layer_view(uint32_t layer) const {
    return layer_views_.empty() ? view_ : layer_views_[layer];
  }
  [[nodiscard]] VkFormat format() const { return format_; }
  [[nodiscard]] uint32_t array_layers() const { return array_layers_; }

 private:
  void Release();

  VkDevice device_ = VK_NULL_HANDLE;
  VmaAllocator allocator_ = VK_NULL_HANDLE;
  VkImage image_ = VK_NULL_HANDLE;
  VkImageView view_ = VK_NULL_HANDLE;
  std::vector<VkImageView> layer_views_;
  VmaAllocation allocation_ = VK_NULL_HANDLE;
  VkFormat format_ = VK_FORMAT_UNDEFINED;
  uint32_t array_layers_ = 1;
};

}  // namespace blue::render

#endif  // BLUE_RENDER_IMAGE_H_
