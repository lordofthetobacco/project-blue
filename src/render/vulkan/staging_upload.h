#ifndef BLUE_RENDER_VULKAN_STAGING_UPLOAD_H_
#define BLUE_RENDER_VULKAN_STAGING_UPLOAD_H_

#include <vulkan/vulkan.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "render/vulkan/buffer.h"
#include "render/vulkan/vulkan_context.h"

namespace blue::render::vulkan {

// The copy layout of a format: rows of blocks, each block `extent` x
// `extent` texels of `bytes` bytes (extent 1 for uncompressed formats).
struct CopyBlock {
  uint32_t extent = 1;
  uint32_t bytes = 0;
};

// Returns the copy layout of `format`; bytes is 0 for formats uploads do
// not handle (depth, packed, ...).
[[nodiscard]] CopyBlock CopyBlockOf(VkFormat format);

// Returns the bytes of mip `level` of an image of `format` whose mip 0 is
// `extent`, in its copy layout (whole blocks; 0 for unhandled formats).
[[nodiscard]] VkDeviceSize CopyByteSize(VkFormat format, VkExtent2D extent,
                                        uint32_t level);

// Streams CPU data into device-local buffers and images through a bounded
// amount of staging memory: two staging chunks of at most kChunkBytes each,
// used in turn, so the CPU fills one while the GPU copies from the other.
// Data larger than a chunk is split (buffers by range, image levels by rows
// of blocks). Commands are recorded into the current chunk's command
// buffer, which cmd() exposes for barriers around the copies; chunks are
// submitted in order on the context's queue, so barriers recorded in one
// chunk order the copies of the next. Finish() (or the destructor) submits
// the rest, blocks until the GPU is done and frees the staging memory.
// Neither copyable nor movable. Not thread-safe.
class StagingUploader {
 public:
  static constexpr VkDeviceSize kChunkBytes = VkDeviceSize{16} << 20;

  // `total_bytes` (the sum of what will be staged, may be approximate)
  // keeps the staging buffers small for small uploads.
  StagingUploader(const VulkanContext& context, VkDeviceSize total_bytes);
  ~StagingUploader();

  StagingUploader(const StagingUploader&) = delete;
  StagingUploader& operator=(const StagingUploader&) = delete;

  // Returns the command buffer the next copy will be recorded into.
  [[nodiscard]] VkCommandBuffer cmd();

  // Records a copy of `bytes` to `buffer` at `offset`.
  void CopyToBuffer(std::span<const std::byte> bytes, VkBuffer buffer,
                    VkDeviceSize offset);
  // Records a copy of `bytes`, mip `level` of array layer `layer` of
  // `image` (whose mip 0 is `extent`; TRANSFER_DST_OPTIMAL when the copy
  // executes) in the copy layout of `format`: tightly packed rows of blocks,
  // top to bottom. A size that does not match is fatal.
  void CopyToImage(std::span<const std::byte> bytes, VkImage image,
                   VkFormat format, VkExtent2D extent, uint32_t level,
                   uint32_t layer);

  // Submits what is recorded, waits for the GPU and frees the staging
  // memory. Nothing may be recorded afterwards.
  void Finish();

 private:
  struct Chunk {
    Buffer staging;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    bool submitted = false;  // Its fence is pending.
  };

  // Returns where `size` bytes (at most the chunk size) can be written in
  // the current chunk, aligned to `alignment`, moving on to the next chunk
  // when the current one is full.
  [[nodiscard]] VkDeviceSize Reserve(VkDeviceSize size, VkDeviceSize alignment);
  // Waits until `chunk` is free and starts recording into it.
  void Begin(Chunk& chunk);
  void Submit(Chunk& chunk);

  const VulkanContext& context_;
  VkDeviceSize chunk_bytes_ = 0;
  VkCommandPool pool_ = VK_NULL_HANDLE;
  std::array<Chunk, 2> chunks_;
  size_t current_ = 0;
  VkDeviceSize used_ = 0;           // Bytes used in the current chunk.
  VkDeviceSize staging_bytes_ = 0;  // Accounted in the context.
  bool recording_ = false;
  bool finished_ = false;
};

}  // namespace blue::render::vulkan

#endif  // BLUE_RENDER_VULKAN_STAGING_UPLOAD_H_
