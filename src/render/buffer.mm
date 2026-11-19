#include "render/buffer.h"

#include <cstring>

#include "core/log.h"
#include "render/metal_check.h"

namespace blue::render {

namespace {

MTLResourceOptions ResourceOptions(Buffer::Access access) {
  switch (access) {
    case Buffer::Access::kGpuOnly:
      return MTLResourceStorageModePrivate;
    case Buffer::Access::kCpuSequentialWrite:
      return MTLResourceStorageModeShared |
             MTLResourceCPUCacheModeWriteCombined;
    case Buffer::Access::kCpuRead:
      return MTLResourceStorageModeShared;
  }
  return MTLResourceStorageModeShared;
}

}  // namespace

Buffer::Buffer(const MetalContext& context, size_t size, Access access,
               const char* label) {
  if (size == 0) {
    core::log::Fatal("Cannot create an empty buffer ({})",
                     label != nullptr ? label : "unlabelled");
  }
  buffer_ = [context.device() newBufferWithLength:size
                                          options:ResourceOptions(access)];
  FatalIfNil(buffer_, nil, "newBufferWithLength");
  buffer_.label = label != nullptr ? @(label) : @"Buffer";
}

Buffer CreateGpuBuffer(const MetalContext& context,
                       std::span<const std::byte> data, const char* label) {
  const size_t size = data.size_bytes();
  Buffer staging(context, size, Buffer::Access::kCpuSequentialWrite,
                 "Staging");
  std::memcpy(staging.mapped(), data.data(), size);

  Buffer buffer(context, size, Buffer::Access::kGpuOnly, label);
  context.SubmitImmediate([&](id<MTLCommandBuffer> command_buffer) {
    const id<MTLBlitCommandEncoder> blit = [command_buffer blitCommandEncoder];
    blit.label = @"Upload buffer";
    [blit copyFromBuffer:staging.handle()
             sourceOffset:0
                 toBuffer:buffer.handle()
        destinationOffset:0
                     size:size];
    [blit endEncoding];
  });
  return buffer;
}

}  // namespace blue::render
