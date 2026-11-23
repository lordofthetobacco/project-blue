#ifndef BLUE_RENDER_SWAPCHAIN_H_
#define BLUE_RENDER_SWAPCHAIN_H_

#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

#include "render/vulkan_context.h"

namespace blue::render {

// Swapchain with its image views and one render-finished semaphore per image.
// Borrows `context`, which must outlive it. Neither copyable nor movable. Not
// thread-safe.
class Swapchain {
 public:
  Swapchain(const VulkanContext& context, VkExtent2D desired_extent,
            bool vsync);
  ~Swapchain();

  Swapchain(const Swapchain&) = delete;
  Swapchain& operator=(const Swapchain&) = delete;

  // Rebuilds the swapchain for `desired_extent`. The caller must ensure the
  // GPU no longer uses the current images. Returns false, leaving the current
  // swapchain untouched, while the surface has a zero extent.
  [[nodiscard]] bool Recreate(VkExtent2D desired_extent, bool vsync);

  [[nodiscard]] VkSwapchainKHR handle() const { return swapchain_; }
  [[nodiscard]] VkFormat format() const { return format_; }
  [[nodiscard]] VkExtent2D extent() const { return extent_; }
  [[nodiscard]] VkPresentModeKHR present_mode() const { return present_mode_; }
  [[nodiscard]] uint32_t min_image_count() const { return min_image_count_; }
  // Whether the images can be copied from (TRANSFER_SRC usage).
  [[nodiscard]] bool readable() const { return readable_; }
  [[nodiscard]] uint32_t image_count() const {
    return static_cast<uint32_t>(images_.size());
  }
  [[nodiscard]] VkImage image(uint32_t index) const { return images_[index]; }
  [[nodiscard]] VkImageView image_view(uint32_t index) const {
    return image_views_[index];
  }

  // Returns the semaphore to signal when rendering to image `index` is done.
  // One per image: the presentation engine may still hold a semaphore of an
  // image until that image is acquired again, so these cannot be per frame in
  // flight.
  [[nodiscard]] VkSemaphore render_finished(uint32_t index) const {
    return render_finished_[index];
  }

 private:
  void Create(const VkSurfaceCapabilitiesKHR& capabilities, VkExtent2D extent,
              bool vsync, VkSwapchainKHR old_swapchain);
  void DestroyImageResources();

  const VulkanContext& context_;
  VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
  VkFormat format_ = VK_FORMAT_UNDEFINED;
  VkColorSpaceKHR color_space_ = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
  VkExtent2D extent_{};
  VkPresentModeKHR present_mode_ = VK_PRESENT_MODE_FIFO_KHR;
  uint32_t min_image_count_ = 0;
  bool readable_ = false;
  std::vector<VkImage> images_;
  std::vector<VkImageView> image_views_;
  std::vector<VkSemaphore> render_finished_;
};

}  // namespace blue::render

#endif  // BLUE_RENDER_SWAPCHAIN_H_
