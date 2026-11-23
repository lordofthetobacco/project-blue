#include "render/vulkan/staging_upload.h"

#include <algorithm>
#include <cstring>

#include "core/log.h"
#include "render/vulkan/image.h"
#include "render/vulkan/vk_check.h"

namespace blue::render::vulkan {

namespace {

// Staging offsets are aligned to a BC block (16 bytes), which also satisfies
// the texel size of every uncompressed format we upload and the 4-byte
// alignment of buffer-to-image copies.
constexpr VkDeviceSize kAlignment = 16;
// Below this much free space a chunk is not worth starting a piece in.
constexpr VkDeviceSize kMinPieceBytes = VkDeviceSize{64} << 10;

VkDeviceSize AlignUp(VkDeviceSize value, VkDeviceSize alignment) {
  return (value + alignment - 1) / alignment * alignment;
}

}  // namespace

CopyBlock CopyBlockOf(VkFormat format) {
  switch (format) {
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
    case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
    case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
    case VK_FORMAT_BC4_UNORM_BLOCK:
    case VK_FORMAT_BC4_SNORM_BLOCK:
      return {.extent = 4, .bytes = 8};
    case VK_FORMAT_BC2_UNORM_BLOCK:
    case VK_FORMAT_BC2_SRGB_BLOCK:
    case VK_FORMAT_BC3_UNORM_BLOCK:
    case VK_FORMAT_BC3_SRGB_BLOCK:
    case VK_FORMAT_BC5_UNORM_BLOCK:
    case VK_FORMAT_BC5_SNORM_BLOCK:
    case VK_FORMAT_BC6H_UFLOAT_BLOCK:
    case VK_FORMAT_BC6H_SFLOAT_BLOCK:
    case VK_FORMAT_BC7_UNORM_BLOCK:
    case VK_FORMAT_BC7_SRGB_BLOCK:
      return {.extent = 4, .bytes = 16};
    default:
      return {.extent = 1, .bytes = TexelSize(format)};
  }
}

VkDeviceSize CopyByteSize(VkFormat format, VkExtent2D extent, uint32_t level) {
  const CopyBlock block = CopyBlockOf(format);
  const uint32_t width = std::max(extent.width >> level, 1u);
  const uint32_t height = std::max(extent.height >> level, 1u);
  return VkDeviceSize{(width + block.extent - 1) / block.extent} *
         ((height + block.extent - 1) / block.extent) * block.bytes;
}

StagingUploader::StagingUploader(const VulkanContext& context,
                                 VkDeviceSize total_bytes)
    : context_(context),
      chunk_bytes_(std::clamp(AlignUp(total_bytes, kAlignment) + kAlignment,
                              kAlignment, kChunkBytes)) {
  const VkCommandPoolCreateInfo pool_info{
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT |
               VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
      .queueFamilyIndex = context_.queue_family(),
  };
  CheckVk(vkCreateCommandPool(context_.device(), &pool_info, nullptr, &pool_));
}

StagingUploader::~StagingUploader() { Finish(); }

VkCommandBuffer StagingUploader::cmd() {
  if (!recording_) {
    Begin(chunks_[current_]);
  }
  return chunks_[current_].cmd;
}

void StagingUploader::CopyToBuffer(std::span<const std::byte> bytes,
                                   VkBuffer buffer, VkDeviceSize offset) {
  VkDeviceSize done = 0;
  while (done < bytes.size_bytes()) {
    const VkDeviceSize free_bytes =
        recording_
            ? chunk_bytes_ - std::min(AlignUp(used_, kAlignment), chunk_bytes_)
            : chunk_bytes_;
    const VkDeviceSize piece =
        std::min(bytes.size_bytes() - done,
                 free_bytes >= kMinPieceBytes ? free_bytes : chunk_bytes_);
    const VkDeviceSize staged = Reserve(piece, kAlignment);
    Chunk& chunk = chunks_[current_];
    std::memcpy(static_cast<std::byte*>(chunk.staging.mapped()) + staged,
                bytes.data() + done, piece);
    const VkBufferCopy region{
        .srcOffset = staged, .dstOffset = offset + done, .size = piece};
    vkCmdCopyBuffer(chunk.cmd, chunk.staging.handle(), buffer, 1, &region);
    done += piece;
  }
}

void StagingUploader::CopyToImage(std::span<const std::byte> bytes,
                                  VkImage image, VkFormat format,
                                  VkExtent2D extent, uint32_t level,
                                  uint32_t layer) {
  const CopyBlock block = CopyBlockOf(format);
  const uint32_t width = std::max(extent.width >> level, 1u);
  const uint32_t height = std::max(extent.height >> level, 1u);
  const VkDeviceSize row_bytes =
      VkDeviceSize{(width + block.extent - 1) / block.extent} * block.bytes;
  const uint32_t rows = (height + block.extent - 1) / block.extent;
  const VkDeviceSize expected = CopyByteSize(format, extent, level);
  if (block.bytes == 0 || bytes.size_bytes() != expected) {
    core::log::Fatal("Image upload of {} bytes, expected {} (format {})",
                     bytes.size_bytes(), expected, static_cast<int>(format));
  }
  if (row_bytes > chunk_bytes_) {
    core::log::Fatal("Image row of {} bytes exceeds the staging chunk",
                     row_bytes);
  }

  uint32_t row = 0;
  while (row < rows) {
    const VkDeviceSize free_bytes =
        recording_
            ? chunk_bytes_ - std::min(AlignUp(used_, kAlignment), chunk_bytes_)
            : chunk_bytes_;
    const VkDeviceSize fit = free_bytes >= row_bytes ? free_bytes / row_bytes
                                                     : chunk_bytes_ / row_bytes;
    const auto count =
        static_cast<uint32_t>(std::min<VkDeviceSize>(rows - row, fit));
    const VkDeviceSize staged = Reserve(row_bytes * count, kAlignment);
    Chunk& chunk = chunks_[current_];
    std::memcpy(static_cast<std::byte*>(chunk.staging.mapped()) + staged,
                bytes.data() + row_bytes * row, row_bytes * count);
    // The last strip ends at the level's edge, which may cut a block.
    const uint32_t top = row * block.extent;
    const uint32_t strip = std::min(count * block.extent, height - top);
    const VkBufferImageCopy2 region{
        .sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
        .bufferOffset = staged,
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, layer, 1},
        .imageOffset = {0, static_cast<int32_t>(top), 0},
        .imageExtent = {width, strip, 1},
    };
    const VkCopyBufferToImageInfo2 copy{
        .sType = VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2,
        .srcBuffer = chunk.staging.handle(),
        .dstImage = image,
        .dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .regionCount = 1,
        .pRegions = &region,
    };
    vkCmdCopyBufferToImage2(chunk.cmd, &copy);
    row += count;
  }
}

VkDeviceSize StagingUploader::Reserve(VkDeviceSize size,
                                      VkDeviceSize alignment) {
  if (!recording_) {
    Begin(chunks_[current_]);
  }
  VkDeviceSize offset = AlignUp(used_, alignment);
  if (offset + size > chunk_bytes_) {
    Submit(chunks_[current_]);
    current_ = (current_ + 1) % chunks_.size();
    Begin(chunks_[current_]);
    offset = 0;
  }
  used_ = offset + size;
  return offset;
}

void StagingUploader::Begin(Chunk& chunk) {
  const VkDevice device = context_.device();
  if (chunk.cmd == VK_NULL_HANDLE) {
    const VkCommandBufferAllocateInfo alloc_info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool_,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    CheckVk(vkAllocateCommandBuffers(device, &alloc_info, &chunk.cmd));
    const VkFenceCreateInfo fence_info{.sType =
                                           VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    CheckVk(vkCreateFence(device, &fence_info, nullptr, &chunk.fence));
    chunk.staging = Buffer(context_.allocator(), chunk_bytes_,
                           VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                           Buffer::Access::kCpuSequentialWrite);
    staging_bytes_ += chunk_bytes_;
    context_.AddStagingBytes(chunk_bytes_);
  }
  if (chunk.submitted) {
    CheckVk(vkWaitForFences(device, 1, &chunk.fence, VK_TRUE, UINT64_MAX));
    CheckVk(vkResetFences(device, 1, &chunk.fence));
    chunk.submitted = false;
  }
  CheckVk(vkResetCommandBuffer(chunk.cmd, 0));
  const VkCommandBufferBeginInfo begin_info{
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
  };
  CheckVk(vkBeginCommandBuffer(chunk.cmd, &begin_info));
  recording_ = true;
  used_ = 0;
}

void StagingUploader::Submit(Chunk& chunk) {
  CheckVk(vkEndCommandBuffer(chunk.cmd));
  const VkCommandBufferSubmitInfo cmd_info{
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
      .commandBuffer = chunk.cmd,
  };
  const VkSubmitInfo2 submit_info{
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
      .commandBufferInfoCount = 1,
      .pCommandBufferInfos = &cmd_info,
  };
  CheckVk(vkQueueSubmit2(context_.queue(), 1, &submit_info, chunk.fence));
  chunk.submitted = true;
  recording_ = false;
}

void StagingUploader::Finish() {
  if (finished_) {
    return;
  }
  finished_ = true;
  if (recording_) {
    Submit(chunks_[current_]);
  }
  const VkDevice device = context_.device();
  for (Chunk& chunk : chunks_) {
    if (chunk.submitted) {
      CheckVk(vkWaitForFences(device, 1, &chunk.fence, VK_TRUE, UINT64_MAX));
    }
    if (chunk.fence != VK_NULL_HANDLE) {
      vkDestroyFence(device, chunk.fence, nullptr);
    }
    chunk = Chunk();  // Frees the staging buffer.
  }
  context_.RemoveStagingBytes(staging_bytes_);
  staging_bytes_ = 0;
  // Frees the command buffers too.
  vkDestroyCommandPool(device, pool_, nullptr);
  pool_ = VK_NULL_HANDLE;
}

}  // namespace blue::render::vulkan
