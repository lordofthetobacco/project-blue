#include "render/Swapchain.hpp"

#include "render/VkCheck.hpp"
#include "render/VulkanContext.hpp"

#include <algorithm>

namespace blue {

namespace {

VkSurfaceFormatKHR chooseSurfaceFormat(VkPhysicalDevice device, VkSurfaceKHR surface)
{
    uint32_t count = 0;
    VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &count, nullptr));
    std::vector<VkSurfaceFormatKHR> formats(count);
    VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &count, formats.data()));

    // An sRGB format lets the hardware apply gamma encoding to our linear shading output.
    for (const VkSurfaceFormatKHR& candidate : formats) {
        const bool srgb = candidate.format == VK_FORMAT_B8G8R8A8_SRGB || candidate.format == VK_FORMAT_R8G8B8A8_SRGB;
        if (srgb && candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return candidate;
        }
    }
    log::warn("No sRGB surface format available, colors will be too dark");
    return formats.front();
}

VkPresentModeKHR choosePresentMode(VkPhysicalDevice device, VkSurfaceKHR surface, bool vsync)
{
    if (vsync) {
        return VK_PRESENT_MODE_FIFO_KHR; // Always supported.
    }

    uint32_t count = 0;
    VK_CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &count, nullptr));
    std::vector<VkPresentModeKHR> modes(count);
    VK_CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &count, modes.data()));

    for (VkPresentModeKHR preferred : {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR}) {
        if (std::ranges::find(modes, preferred) != modes.end()) {
            return preferred;
        }
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

VkSurfaceCapabilitiesKHR querySurfaceCapabilities(const VulkanContext& context)
{
    VkSurfaceCapabilitiesKHR capabilities{};
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(context.physicalDevice(), context.surface(), &capabilities));
    return capabilities;
}

VkExtent2D chooseExtent(const VkSurfaceCapabilitiesKHR& capabilities, VkExtent2D desired)
{
    if (capabilities.currentExtent.width != UINT32_MAX) {
        return capabilities.currentExtent;
    }
    return {
        std::clamp(desired.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width),
        std::clamp(desired.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height),
    };
}

} // namespace

Swapchain::Swapchain(const VulkanContext& context, VkExtent2D desiredExtent, bool vsync)
    : context_(context)
{
    const VkSurfaceFormatKHR surfaceFormat = chooseSurfaceFormat(context_.physicalDevice(), context_.surface());
    format_ = surfaceFormat.format;
    colorSpace_ = surfaceFormat.colorSpace;

    const VkSurfaceCapabilitiesKHR capabilities = querySurfaceCapabilities(context_);
    const VkExtent2D extent = chooseExtent(capabilities, desiredExtent);
    if (extent.width == 0 || extent.height == 0) {
        log::fatal("Cannot create a swapchain for a zero-sized surface");
    }
    create(capabilities, extent, vsync, VK_NULL_HANDLE);
}

Swapchain::~Swapchain()
{
    destroyImageResources();
    vkDestroySwapchainKHR(context_.device(), swapchain_, nullptr);
}

bool Swapchain::recreate(VkExtent2D desiredExtent, bool vsync)
{
    // The surface can report a zero extent (e.g. minimized on Windows) even when the
    // window still has a size; a swapchain cannot be created until that changes.
    const VkSurfaceCapabilitiesKHR capabilities = querySurfaceCapabilities(context_);
    const VkExtent2D extent = chooseExtent(capabilities, desiredExtent);
    if (extent.width == 0 || extent.height == 0) {
        return false;
    }

    const VkSwapchainKHR oldSwapchain = swapchain_;
    destroyImageResources();
    create(capabilities, extent, vsync, oldSwapchain);
    vkDestroySwapchainKHR(context_.device(), oldSwapchain, nullptr);
    return true;
}

void Swapchain::create(const VkSurfaceCapabilitiesKHR& capabilities, VkExtent2D extent, bool vsync,
                       VkSwapchainKHR oldSwapchain)
{
    const VkDevice device = context_.device();

    extent_ = extent;
    presentMode_ = choosePresentMode(context_.physicalDevice(), context_.surface(), vsync);

    // One image above the minimum so we never block waiting on the presentation engine.
    minImageCount_ = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount > 0) {
        minImageCount_ = std::min(minImageCount_, capabilities.maxImageCount);
    }

    const VkSwapchainCreateInfoKHR createInfo{
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = context_.surface(),
        .minImageCount = minImageCount_,
        .imageFormat = format_,
        .imageColorSpace = colorSpace_,
        .imageExtent = extent_,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = capabilities.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = presentMode_,
        .clipped = VK_TRUE,
        .oldSwapchain = oldSwapchain,
    };
    VK_CHECK(vkCreateSwapchainKHR(device, &createInfo, nullptr, &swapchain_));

    uint32_t imageCount = 0;
    VK_CHECK(vkGetSwapchainImagesKHR(device, swapchain_, &imageCount, nullptr));
    images_.resize(imageCount);
    VK_CHECK(vkGetSwapchainImagesKHR(device, swapchain_, &imageCount, images_.data()));

    imageViews_.resize(imageCount);
    renderFinished_.resize(imageCount);
    for (uint32_t i = 0; i < imageCount; ++i) {
        const VkImageViewCreateInfo viewInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = images_[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = format_,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
        VK_CHECK(vkCreateImageView(device, &viewInfo, nullptr, &imageViews_[i]));

        const VkSemaphoreCreateInfo semaphoreInfo{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CHECK(vkCreateSemaphore(device, &semaphoreInfo, nullptr, &renderFinished_[i]));
    }
}

void Swapchain::destroyImageResources()
{
    for (VkSemaphore semaphore : renderFinished_) {
        vkDestroySemaphore(context_.device(), semaphore, nullptr);
    }
    for (VkImageView view : imageViews_) {
        vkDestroyImageView(context_.device(), view, nullptr);
    }
    renderFinished_.clear();
    imageViews_.clear();
    images_.clear();
}

} // namespace blue
