#ifndef BLUE_RENDER_BUFFER_H_
#define BLUE_RENDER_BUFFER_H_

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <cstddef>
#include <span>

#include "render/vulkan_context.h"

namespace blue::render {

// GPU buffer backed by a VMA allocation. Move-only; a moved-from buffer is
// empty. Not thread-safe.
class Buffer {
 public:
  enum class Access {
    kGpuOnly,             // Device-local, filled through a staging upload.
    kCpuSequentialWrite,  // Host-visible, persistently mapped, written often.
  };

  Buffer() = default;
  Buffer(VmaAllocator allocator, VkDeviceSize size, VkBufferUsageFlags usage,
         Access access);
  ~Buffer();

  Buffer(const Buffer&) = delete;
  Buffer& operator=(const Buffer&) = delete;
  Buffer(Buffer&& other) noexcept;
  Buffer& operator=(Buffer&& other) noexcept;

  [[nodiscard]] VkBuffer handle() const { return buffer_; }
  [[nodiscard]] VkDeviceSize size() const { return size_; }
  // Returns the persistent mapping, or nullptr unless the buffer was created
  // with Access::kCpuSequentialWrite.
  [[nodiscard]] void* mapped() const { return mapped_; }

 private:
  void Release();

  VmaAllocator allocator_ = VK_NULL_HANDLE;
  VkBuffer buffer_ = VK_NULL_HANDLE;
  VmaAllocation allocation_ = VK_NULL_HANDLE;
  VkDeviceSize size_ = 0;
  void* mapped_ = nullptr;
};

// Creates a device-local buffer and fills it with `data` through a staging
// buffer. Blocks until the upload has finished.
[[nodiscard]] Buffer CreateGpuBuffer(const VulkanContext& context,
                                     std::span<const std::byte> data,
                                     VkBufferUsageFlags usage);

}  // namespace blue::render

#endif  // BLUE_RENDER_BUFFER_H_
