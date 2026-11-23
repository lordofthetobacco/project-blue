#include "render/staging_upload.h"

#include <algorithm>
#include <cstring>

#include "core/log.h"
#include "render/image.h"
#include "render/metal_check.h"

namespace blue::render {

namespace {

// Staging offsets are aligned to a BC block (16 bytes), which also satisfies
// the texel size of every uncompressed format we upload (a blit's source
// offset must be a multiple of it).
constexpr size_t kAlignment = 16;
// Below this much free space a chunk is not worth starting a piece in.
constexpr size_t kMinPieceBytes = size_t{64} << 10;

size_t AlignUp(size_t value, size_t alignment) {
  return (value + alignment - 1) / alignment * alignment;
}

}  // namespace

CopyBlock CopyBlockOf(MTLPixelFormat format) {
  switch (format) {
    case MTLPixelFormatBC1_RGBA:
    case MTLPixelFormatBC1_RGBA_sRGB:
    case MTLPixelFormatBC4_RUnorm:
    case MTLPixelFormatBC4_RSnorm:
      return {.extent = 4, .bytes = 8};
    case MTLPixelFormatBC2_RGBA:
    case MTLPixelFormatBC2_RGBA_sRGB:
    case MTLPixelFormatBC3_RGBA:
    case MTLPixelFormatBC3_RGBA_sRGB:
    case MTLPixelFormatBC5_RGUnorm:
    case MTLPixelFormatBC5_RGSnorm:
    case MTLPixelFormatBC6H_RGBUfloat:
    case MTLPixelFormatBC6H_RGBFloat:
    case MTLPixelFormatBC7_RGBAUnorm:
    case MTLPixelFormatBC7_RGBAUnorm_sRGB:
      return {.extent = 4, .bytes = 16};
    default:
      return {.extent = 1, .bytes = TexelSize(format)};
  }
}

size_t CopyByteSize(MTLPixelFormat format, core::Extent2D extent,
                    uint32_t level) {
  const CopyBlock block = CopyBlockOf(format);
  const uint32_t width = std::max(extent.width >> level, 1u);
  const uint32_t height = std::max(extent.height >> level, 1u);
  return size_t{(width + block.extent - 1) / block.extent} *
         ((height + block.extent - 1) / block.extent) * block.bytes;
}

StagingUploader::StagingUploader(const MetalContext& context,
                                 size_t total_bytes)
    : context_(context),
      chunk_bytes_(std::clamp(AlignUp(total_bytes, kAlignment) + kAlignment,
                              kAlignment, kChunkBytes)) {}

StagingUploader::~StagingUploader() { Finish(); }

id<MTL4ComputeCommandEncoder> StagingUploader::encoder() {
  if (!recording_) {
    Begin(chunks_[current_]);
  }
  return chunks_[current_].encoder;
}

size_t StagingUploader::FreeBytes() const {
  return recording_
             ? chunk_bytes_ - std::min(AlignUp(used_, kAlignment), chunk_bytes_)
             : chunk_bytes_;
}

void StagingUploader::CopyToBuffer(std::span<const std::byte> bytes,
                                   id<MTLBuffer> buffer, size_t offset) {
  size_t done = 0;
  while (done < bytes.size_bytes()) {
    const size_t free_bytes = FreeBytes();
    const size_t piece =
        std::min(bytes.size_bytes() - done,
                 free_bytes >= kMinPieceBytes ? free_bytes : chunk_bytes_);
    const size_t staged = Reserve(piece, kAlignment);
    const Chunk& chunk = chunks_[current_];
    std::memcpy(static_cast<std::byte*>(chunk.staging.mapped()) + staged,
                bytes.data() + done, piece);
    [chunk.encoder copyFromBuffer:chunk.staging.handle()
                  sourceOffset:staged
                      toBuffer:buffer
             destinationOffset:offset + done
                          size:piece];
    done += piece;
  }
}

void StagingUploader::CopyToImage(std::span<const std::byte> bytes,
                                  id<MTLTexture> texture, uint32_t level,
                                  uint32_t layer) {
  const MTLPixelFormat format = texture.pixelFormat;
  const core::Extent2D extent{static_cast<uint32_t>(texture.width),
                              static_cast<uint32_t>(texture.height)};
  const CopyBlock block = CopyBlockOf(format);
  const uint32_t width = std::max(extent.width >> level, 1u);
  const uint32_t height = std::max(extent.height >> level, 1u);
  const size_t row_bytes =
      size_t{(width + block.extent - 1) / block.extent} * block.bytes;
  const uint32_t rows = (height + block.extent - 1) / block.extent;
  const size_t expected = CopyByteSize(format, extent, level);
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
    const size_t free_bytes = FreeBytes();
    const size_t fit = free_bytes >= row_bytes ? free_bytes / row_bytes
                                               : chunk_bytes_ / row_bytes;
    const auto count = static_cast<uint32_t>(std::min<size_t>(rows - row, fit));
    const size_t staged = Reserve(row_bytes * count, kAlignment);
    const Chunk& chunk = chunks_[current_];
    std::memcpy(static_cast<std::byte*>(chunk.staging.mapped()) + staged,
                bytes.data() + row_bytes * row, row_bytes * count);
    // The last strip ends at the level's edge, which may cut a block: the
    // source size is in texels of the level, not in whole blocks.
    const uint32_t top = row * block.extent;
    const uint32_t strip = std::min(count * block.extent, height - top);
    [chunk.encoder copyFromBuffer:chunk.staging.handle()
                  sourceOffset:staged
             sourceBytesPerRow:row_bytes
           sourceBytesPerImage:row_bytes * count
                    sourceSize:MTLSizeMake(width, strip, 1)
                     toTexture:texture
              destinationSlice:layer
              destinationLevel:level
             destinationOrigin:MTLOriginMake(0, top, 0)];
    row += count;
  }
}

size_t StagingUploader::Reserve(size_t size, size_t alignment) {
  if (!recording_) {
    Begin(chunks_[current_]);
  }
  size_t offset = AlignUp(used_, alignment);
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
  if (finished_) {
    core::log::Fatal("Staging upload recorded after Finish()");
  }
  if (chunk.staging.handle() == nil) {
    chunk.staging = Buffer(context_, chunk_bytes_,
                           Buffer::Access::kCpuSequentialWrite, "Staging");
    staging_bytes_ += chunk_bytes_;
    context_.AddStagingBytes(chunk_bytes_);
    chunk.allocator = [context_.device() newCommandAllocator];
    FatalIfNil(chunk.allocator, nil, "newCommandAllocator");
    chunk.cmd = [context_.device() newCommandBuffer];
    FatalIfNil(chunk.cmd, nil, "newCommandBuffer");
    chunk.cmd.label = @"Staging upload";
  }
  Wait(chunk);
  // The chunk's previous commands have completed: their memory is free.
  [chunk.allocator reset];
  [chunk.cmd beginCommandBufferWithAllocator:chunk.allocator];
  // The member keeps the encoder; the pool drains what else its creation
  // autoreleased.
  @autoreleasepool {
    chunk.encoder = [chunk.cmd computeCommandEncoder];
    FatalIfNil(chunk.encoder, nil, "computeCommandEncoder");
    chunk.encoder.label = @"Staging upload";
    // The copies and mip generation of the chunk committed before must be
    // visible to this one's (an image may span both).
    [chunk.encoder barrierAfterQueueStages:MTLStageBlit
                              beforeStages:MTLStageBlit
                         visibilityOptions:MTL4VisibilityOptionDevice];
  }
  recording_ = true;
  used_ = 0;
}

void StagingUploader::Submit(Chunk& chunk) {
  [chunk.encoder endEncoding];
  chunk.encoder = nil;
  [chunk.cmd endCommandBuffer];
  chunk.submitted = context_.Commit(chunk.cmd, "staging upload");
  recording_ = false;
}

void StagingUploader::Wait(Chunk& chunk) {
  if (chunk.submitted == nullptr) {
    return;
  }
  chunk.submitted->Wait();
  chunk.submitted.reset();
}

void StagingUploader::Finish() {
  if (finished_) {
    return;
  }
  finished_ = true;
  if (recording_) {
    Submit(chunks_[current_]);
  }
  for (Chunk& chunk : chunks_) {
    Wait(chunk);
    chunk = Chunk();  // Frees the staging buffer.
  }
  context_.RemoveStagingBytes(staging_bytes_);
  staging_bytes_ = 0;
}

}  // namespace blue::render
