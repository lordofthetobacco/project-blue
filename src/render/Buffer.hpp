#pragma once

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <cstddef>
#include <span>

namespace blue {

class VulkanContext;

// GPU buffer backed by a VMA allocation. Move-only.
class Buffer {
public:
    enum class Access {
        GpuOnly,            // Device-local, filled through a staging upload.
        CpuSequentialWrite, // Host-visible and persistently mapped, written every frame.
    };

    Buffer() = default;
    Buffer(VmaAllocator allocator, VkDeviceSize size, VkBufferUsageFlags usage, Access access);
    ~Buffer();

    Buffer(Buffer&& other) noexcept;
    Buffer& operator=(Buffer&& other) noexcept;
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;

    [[nodiscard]] VkBuffer handle() const { return buffer_; }
    [[nodiscard]] VkDeviceSize size() const { return size_; }
    [[nodiscard]] void* mapped() const { return mapped_; }

private:
    void release();

    VmaAllocator allocator_ = VK_NULL_HANDLE;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = VK_NULL_HANDLE;
    VkDeviceSize size_ = 0;
    void* mapped_ = nullptr;
};

// Creates a device-local buffer and fills it with `data` through a staging buffer.
[[nodiscard]] Buffer createGpuBuffer(const VulkanContext& context, std::span<const std::byte> data,
                                     VkBufferUsageFlags usage);

} // namespace blue
