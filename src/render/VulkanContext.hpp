#pragma once

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <functional>
#include <string_view>

namespace blue {

class Window;

// Core Vulkan objects shared by the whole renderer: instance, surface, device,
// the single graphics+present queue and the memory allocator.
class VulkanContext {
public:
    static constexpr uint32_t kApiVersion = VK_API_VERSION_1_3;

    explicit VulkanContext(const Window& window);
    ~VulkanContext();

    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;

    [[nodiscard]] VkInstance instance() const { return instance_; }
    [[nodiscard]] VkSurfaceKHR surface() const { return surface_; }
    [[nodiscard]] VkPhysicalDevice physicalDevice() const { return physicalDevice_; }
    [[nodiscard]] VkDevice device() const { return device_; }
    [[nodiscard]] VkQueue queue() const { return queue_; }
    [[nodiscard]] uint32_t queueFamily() const { return queueFamily_; }
    [[nodiscard]] VmaAllocator allocator() const { return allocator_; }
    [[nodiscard]] const VkPhysicalDeviceProperties& properties() const { return properties_; }
    [[nodiscard]] std::string_view deviceName() const { return properties_.deviceName; }
    [[nodiscard]] uint32_t timestampValidBits() const { return timestampValidBits_; }

    // Records commands with `record` and blocks until the GPU has executed them.
    // Intended for setup work such as resource uploads, not per-frame use.
    void submitImmediate(const std::function<void(VkCommandBuffer)>& record) const;

    void waitIdle() const;

private:
    void createInstance(const Window& window);
    void selectPhysicalDevice();
    void createDevice();
    void createAllocator();

    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties_{};
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queueFamily_ = 0;
    uint32_t timestampValidBits_ = 0;
    VmaAllocator allocator_ = VK_NULL_HANDLE;
    VkCommandPool immediatePool_ = VK_NULL_HANDLE;
};

} // namespace blue
