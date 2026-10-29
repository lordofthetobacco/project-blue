#include "render/Buffer.hpp"

#include "render/VkCheck.hpp"
#include "render/VulkanContext.hpp"

#include <cstring>
#include <utility>

namespace blue {

Buffer::Buffer(VmaAllocator allocator, VkDeviceSize size, VkBufferUsageFlags usage, Access access)
    : allocator_(allocator)
    , size_(size)
{
    const VkBufferCreateInfo bufferInfo{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };

    VmaAllocationCreateInfo allocInfo{.usage = VMA_MEMORY_USAGE_AUTO};
    if (access == Access::CpuSequentialWrite) {
        allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    }

    VmaAllocationInfo result{};
    VK_CHECK(vmaCreateBuffer(allocator_, &bufferInfo, &allocInfo, &buffer_, &allocation_, &result));
    mapped_ = result.pMappedData;
}

Buffer::~Buffer()
{
    release();
}

Buffer::Buffer(Buffer&& other) noexcept
    : allocator_(std::exchange(other.allocator_, VK_NULL_HANDLE))
    , buffer_(std::exchange(other.buffer_, VK_NULL_HANDLE))
    , allocation_(std::exchange(other.allocation_, VK_NULL_HANDLE))
    , size_(std::exchange(other.size_, 0))
    , mapped_(std::exchange(other.mapped_, nullptr))
{
}

Buffer& Buffer::operator=(Buffer&& other) noexcept
{
    if (this != &other) {
        release();
        allocator_ = std::exchange(other.allocator_, VK_NULL_HANDLE);
        buffer_ = std::exchange(other.buffer_, VK_NULL_HANDLE);
        allocation_ = std::exchange(other.allocation_, VK_NULL_HANDLE);
        size_ = std::exchange(other.size_, 0);
        mapped_ = std::exchange(other.mapped_, nullptr);
    }
    return *this;
}

void Buffer::release()
{
    if (buffer_ != VK_NULL_HANDLE) {
        vmaDestroyBuffer(allocator_, buffer_, allocation_);
        buffer_ = VK_NULL_HANDLE;
        allocation_ = VK_NULL_HANDLE;
    }
}

Buffer createGpuBuffer(const VulkanContext& context, std::span<const std::byte> data, VkBufferUsageFlags usage)
{
    const VkDeviceSize size = data.size_bytes();

    Buffer staging(context.allocator(), size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, Buffer::Access::CpuSequentialWrite);
    std::memcpy(staging.mapped(), data.data(), size);

    Buffer buffer(context.allocator(), size, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, Buffer::Access::GpuOnly);

    context.submitImmediate([&](VkCommandBuffer cmd) {
        const VkBufferCopy region{.size = size};
        vkCmdCopyBuffer(cmd, staging.handle(), buffer.handle(), 1, &region);
    });
    return buffer;
}

} // namespace blue
