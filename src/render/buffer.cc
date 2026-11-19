#include "render/buffer.h"

#include <utility>

#include "render/staging_upload.h"
#include "render/vk_check.h"

namespace blue::render {

Buffer::Buffer(VmaAllocator allocator, VkDeviceSize size,
               VkBufferUsageFlags usage, Access access)
    : allocator_(allocator), size_(size) {
  const VkBufferCreateInfo buffer_info{
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = size,
      .usage = usage,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
  };

  VmaAllocationCreateInfo alloc_info{.usage = VMA_MEMORY_USAGE_AUTO};
  if (access == Access::kCpuSequentialWrite) {
    alloc_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                       VMA_ALLOCATION_CREATE_MAPPED_BIT;
  } else if (access == Access::kCpuRead) {
    alloc_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                       VMA_ALLOCATION_CREATE_MAPPED_BIT;
  }

  VmaAllocationInfo result{};
  CheckVk(vmaCreateBuffer(allocator_, &buffer_info, &alloc_info, &buffer_,
                          &allocation_, &result));
  mapped_ = result.pMappedData;
}

Buffer::~Buffer() { Release(); }

Buffer::Buffer(Buffer&& other) noexcept
    : allocator_(std::exchange(other.allocator_, VK_NULL_HANDLE)),
      buffer_(std::exchange(other.buffer_, VK_NULL_HANDLE)),
      allocation_(std::exchange(other.allocation_, VK_NULL_HANDLE)),
      size_(std::exchange(other.size_, 0)),
      mapped_(std::exchange(other.mapped_, nullptr)) {}

Buffer& Buffer::operator=(Buffer&& other) noexcept {
  if (this != &other) {
    Release();
    allocator_ = std::exchange(other.allocator_, VK_NULL_HANDLE);
    buffer_ = std::exchange(other.buffer_, VK_NULL_HANDLE);
    allocation_ = std::exchange(other.allocation_, VK_NULL_HANDLE);
    size_ = std::exchange(other.size_, 0);
    mapped_ = std::exchange(other.mapped_, nullptr);
  }
  return *this;
}

void Buffer::InvalidateForRead() const {
  CheckVk(vmaInvalidateAllocation(allocator_, allocation_, 0, VK_WHOLE_SIZE));
}

void Buffer::Release() {
  if (buffer_ != VK_NULL_HANDLE) {
    vmaDestroyBuffer(allocator_, buffer_, allocation_);
    buffer_ = VK_NULL_HANDLE;
    allocation_ = VK_NULL_HANDLE;
  }
}

Buffer CreateGpuBuffer(const VulkanContext& context,
                       std::span<const std::byte> data,
                       VkBufferUsageFlags usage) {
  Buffer buffer(context.allocator(), data.size_bytes(),
                usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                Buffer::Access::kGpuOnly);
  StagingUploader uploader(context, data.size_bytes());
  uploader.CopyToBuffer(data, buffer.handle(), 0);
  uploader.Finish();
  return buffer;
}

}  // namespace blue::render
