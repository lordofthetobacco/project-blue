#include "render/swapchain.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#include "core/log.h"
#include "render/vk_check.h"

namespace blue::render {

namespace {

VkSurfaceFormatKHR ChooseSurfaceFormat(VkPhysicalDevice device,
                                       VkSurfaceKHR surface) {
  uint32_t count = 0;
  CheckVk(
      vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &count, nullptr));
  std::vector<VkSurfaceFormatKHR> formats(count);
  CheckVk(vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &count,
                                               formats.data()));

  // An sRGB format lets the hardware apply gamma encoding to our linear
  // shading output.
  for (const VkSurfaceFormatKHR& candidate : formats) {
    const bool srgb = candidate.format == VK_FORMAT_B8G8R8A8_SRGB ||
                      candidate.format == VK_FORMAT_R8G8B8A8_SRGB;
    if (srgb && candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
      return candidate;
    }
  }
  core::log::Warn("No sRGB surface format available, colors will be too dark");
  return formats.front();
}

VkPresentModeKHR ChoosePresentMode(VkPhysicalDevice device,
                                   VkSurfaceKHR surface, bool vsync) {
  if (vsync) {
    return VK_PRESENT_MODE_FIFO_KHR;  // Always supported.
  }

  uint32_t count = 0;
  CheckVk(vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &count,
                                                    nullptr));
  std::vector<VkPresentModeKHR> modes(count);
  CheckVk(vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &count,
                                                    modes.data()));

  for (VkPresentModeKHR preferred :
       {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR}) {
    if (std::ranges::find(modes, preferred) != modes.end()) {
      return preferred;
    }
  }
  return VK_PRESENT_MODE_FIFO_KHR;
}

VkSurfaceCapabilitiesKHR QuerySurfaceCapabilities(
    const VulkanContext& context) {
  VkSurfaceCapabilitiesKHR capabilities{};
  CheckVk(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
      context.physical_device(), context.surface(), &capabilities));
  return capabilities;
}

VkExtent2D ChooseExtent(const VkSurfaceCapabilitiesKHR& capabilities,
                        VkExtent2D desired) {
  if (capabilities.currentExtent.width != UINT32_MAX) {
    return capabilities.currentExtent;
  }
  return {
      std::clamp(desired.width, capabilities.minImageExtent.width,
                 capabilities.maxImageExtent.width),
      std::clamp(desired.height, capabilities.minImageExtent.height,
                 capabilities.maxImageExtent.height),
  };
}

}  // namespace

Swapchain::Swapchain(const VulkanContext& context, VkExtent2D desired_extent,
                     bool vsync)
    : context_(context) {
  const VkSurfaceFormatKHR surface_format =
      ChooseSurfaceFormat(context_.physical_device(), context_.surface());
  format_ = surface_format.format;
  color_space_ = surface_format.colorSpace;

  const VkSurfaceCapabilitiesKHR capabilities =
      QuerySurfaceCapabilities(context_);
  const VkExtent2D extent = ChooseExtent(capabilities, desired_extent);
  if (extent.width == 0 || extent.height == 0) {
    core::log::Fatal("Cannot create a swapchain for a zero-sized surface");
  }
  Create(capabilities, extent, vsync, VK_NULL_HANDLE);
}

Swapchain::~Swapchain() {
  DestroyImageResources();
  vkDestroySwapchainKHR(context_.device(), swapchain_, nullptr);
}

bool Swapchain::Recreate(VkExtent2D desired_extent, bool vsync) {
  // The surface can report a zero extent (e.g. minimized on Windows) even when
  // the window still has a size; a swapchain cannot be created until that
  // changes.
  const VkSurfaceCapabilitiesKHR capabilities =
      QuerySurfaceCapabilities(context_);
  const VkExtent2D extent = ChooseExtent(capabilities, desired_extent);
  if (extent.width == 0 || extent.height == 0) {
    return false;
  }

  const VkSwapchainKHR old_swapchain = swapchain_;
  DestroyImageResources();
  Create(capabilities, extent, vsync, old_swapchain);
  vkDestroySwapchainKHR(context_.device(), old_swapchain, nullptr);
  return true;
}

void Swapchain::Create(const VkSurfaceCapabilitiesKHR& capabilities,
                       VkExtent2D extent, bool vsync,
                       VkSwapchainKHR old_swapchain) {
  const VkDevice device = context_.device();

  extent_ = extent;
  present_mode_ =
      ChoosePresentMode(context_.physical_device(), context_.surface(), vsync);

  // One image above the minimum so we never block waiting on the presentation
  // engine.
  min_image_count_ = capabilities.minImageCount + 1;
  if (capabilities.maxImageCount > 0) {
    min_image_count_ = std::min(min_image_count_, capabilities.maxImageCount);
  }

  // Copies of the window output (Renderer::RequestOutputReadback()) need
  // TRANSFER_SRC; every desktop driver offers it.
  readable_ = (capabilities.supportedUsageFlags &
               VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
  const VkImageUsageFlags usage =
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
      (readable_ ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : VkImageUsageFlags{0});

  const VkSwapchainCreateInfoKHR create_info{
      .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
      .surface = context_.surface(),
      .minImageCount = min_image_count_,
      .imageFormat = format_,
      .imageColorSpace = color_space_,
      .imageExtent = extent_,
      .imageArrayLayers = 1,
      .imageUsage = usage,
      .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .preTransform = capabilities.currentTransform,
      .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
      .presentMode = present_mode_,
      .clipped = VK_TRUE,
      .oldSwapchain = old_swapchain,
  };
  CheckVk(vkCreateSwapchainKHR(device, &create_info, nullptr, &swapchain_));

  uint32_t image_count = 0;
  CheckVk(vkGetSwapchainImagesKHR(device, swapchain_, &image_count, nullptr));
  images_.resize(image_count);
  CheckVk(vkGetSwapchainImagesKHR(device, swapchain_, &image_count,
                                  images_.data()));

  image_views_.resize(image_count);
  render_finished_.resize(image_count);
  for (uint32_t i = 0; i < image_count; ++i) {
    const VkImageViewCreateInfo view_info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = images_[i],
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format_,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    CheckVk(vkCreateImageView(device, &view_info, nullptr, &image_views_[i]));

    const VkSemaphoreCreateInfo semaphore_info{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    CheckVk(vkCreateSemaphore(device, &semaphore_info, nullptr,
                              &render_finished_[i]));
  }
}

void Swapchain::DestroyImageResources() {
  for (VkSemaphore semaphore : render_finished_) {
    vkDestroySemaphore(context_.device(), semaphore, nullptr);
  }
  for (VkImageView view : image_views_) {
    vkDestroyImageView(context_.device(), view, nullptr);
  }
  render_finished_.clear();
  image_views_.clear();
  images_.clear();
}

}  // namespace blue::render
