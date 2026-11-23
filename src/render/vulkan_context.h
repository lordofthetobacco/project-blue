#ifndef BLUE_RENDER_VULKAN_CONTEXT_H_
#define BLUE_RENDER_VULKAN_CONTEXT_H_

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <functional>
#include <string_view>

#include "platform/window.h"

namespace blue::render {

// Core Vulkan objects shared by the whole renderer: instance, surface,
// device, the single graphics+present queue and the memory allocator. Startup
// failures (no suitable GPU, ...) are fatal. The window owns the native
// handle behind the surface and must outlive the context. Neither copyable
// nor movable. Not thread-safe.
class VulkanContext {
 public:
  static constexpr uint32_t kApiVersion = VK_API_VERSION_1_3;

  explicit VulkanContext(const platform::Window& window);
  ~VulkanContext();

  VulkanContext(const VulkanContext&) = delete;
  VulkanContext& operator=(const VulkanContext&) = delete;

  [[nodiscard]] VkInstance instance() const { return instance_; }
  [[nodiscard]] VkSurfaceKHR surface() const { return surface_; }
  [[nodiscard]] VkPhysicalDevice physical_device() const {
    return physical_device_;
  }
  [[nodiscard]] VkDevice device() const { return device_; }
  [[nodiscard]] VkQueue queue() const { return queue_; }
  [[nodiscard]] uint32_t queue_family() const { return queue_family_; }
  [[nodiscard]] VmaAllocator allocator() const { return allocator_; }
  [[nodiscard]] const VkPhysicalDeviceProperties& properties() const {
    return properties_;
  }
  [[nodiscard]] std::string_view device_name() const {
    return properties_.deviceName;
  }
  [[nodiscard]] uint32_t timestamp_valid_bits() const {
    return timestamp_valid_bits_;
  }
  // Whether anisotropic filtering is enabled (the samplerAnisotropy feature
  // is optional); the limit is properties().limits.maxSamplerAnisotropy.
  [[nodiscard]] bool sampler_anisotropy() const { return sampler_anisotropy_; }
  // Whether VK_EXT_memory_budget is enabled (it is optional): the allocator
  // then reports the driver's per-process heap usage and budget.
  [[nodiscard]] bool memory_budget() const { return memory_budget_; }

  // Upload staging memory allocated right now and its high-water mark, in
  // bytes (see StagingUploader).
  struct StagingUsage {
    VkDeviceSize current = 0;
    VkDeviceSize peak = 0;
  };
  [[nodiscard]] StagingUsage staging_usage() const { return staging_usage_; }
  // Bookkeeping for StagingUploader: `bytes` of staging memory were created
  // (Add) or destroyed (Remove).
  void AddStagingBytes(VkDeviceSize bytes) const;
  void RemoveStagingBytes(VkDeviceSize bytes) const;

  // Records commands with `record` and blocks until the GPU has executed
  // them. Intended for setup work such as resource uploads, not per-frame use.
  void SubmitImmediate(
      const std::function<void(VkCommandBuffer)>& record) const;

  // Blocks until the device has finished all submitted work.
  void WaitIdle() const;

 private:
  void CreateInstance(const platform::Window& window);
  void SelectPhysicalDevice();
  void CreateDevice();
  void CreateAllocator();

  VkInstance instance_ = VK_NULL_HANDLE;
  VkDebugUtilsMessengerEXT debug_messenger_ = VK_NULL_HANDLE;
  VkSurfaceKHR surface_ = VK_NULL_HANDLE;
  VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
  VkPhysicalDeviceProperties properties_{};
  VkDevice device_ = VK_NULL_HANDLE;
  VkQueue queue_ = VK_NULL_HANDLE;
  uint32_t queue_family_ = 0;
  uint32_t timestamp_valid_bits_ = 0;
  bool sampler_anisotropy_ = false;
  bool memory_budget_ = false;
  // Statistics only; uploads update them through a const context.
  mutable StagingUsage staging_usage_;
  VmaAllocator allocator_ = VK_NULL_HANDLE;
  VkCommandPool immediate_pool_ = VK_NULL_HANDLE;
};

}  // namespace blue::render

#endif  // BLUE_RENDER_VULKAN_CONTEXT_H_
