#pragma once

#include <vulkan/vulkan.h>

#include <vector>

namespace blue {

class VulkanContext;

class Swapchain {
public:
    Swapchain(const VulkanContext& context, VkExtent2D desiredExtent, bool vsync);
    ~Swapchain();

    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;

    // Caller must ensure the GPU no longer uses the current images. Returns false, leaving
    // the current swapchain untouched, while the surface has a zero extent.
    [[nodiscard]] bool recreate(VkExtent2D desiredExtent, bool vsync);

    [[nodiscard]] VkSwapchainKHR handle() const { return swapchain_; }
    [[nodiscard]] VkFormat format() const { return format_; }
    [[nodiscard]] VkExtent2D extent() const { return extent_; }
    [[nodiscard]] VkPresentModeKHR presentMode() const { return presentMode_; }
    [[nodiscard]] uint32_t minImageCount() const { return minImageCount_; }
    [[nodiscard]] uint32_t imageCount() const { return static_cast<uint32_t>(images_.size()); }
    [[nodiscard]] VkImage image(uint32_t index) const { return images_[index]; }
    [[nodiscard]] VkImageView imageView(uint32_t index) const { return imageViews_[index]; }

    // One per image: the presentation engine may still hold a semaphore of an image
    // until that image is acquired again, so these cannot be per frame in flight.
    [[nodiscard]] VkSemaphore renderFinished(uint32_t index) const { return renderFinished_[index]; }

private:
    void create(const VkSurfaceCapabilitiesKHR& capabilities, VkExtent2D extent, bool vsync,
                VkSwapchainKHR oldSwapchain);
    void destroyImageResources();

    const VulkanContext& context_;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR colorSpace_ = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D extent_{};
    VkPresentModeKHR presentMode_ = VK_PRESENT_MODE_FIFO_KHR;
    uint32_t minImageCount_ = 0;
    std::vector<VkImage> images_;
    std::vector<VkImageView> imageViews_;
    std::vector<VkSemaphore> renderFinished_;
};

} // namespace blue
