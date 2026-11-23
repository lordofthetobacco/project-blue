#include "render/buffer.h"

#include "core/log.h"
#include "render/metal_check.h"
#include "render/staging_upload.h"

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
  resident_ = Resident(context, buffer_);
}

Buffer CreateGpuBuffer(const MetalContext& context,
                       std::span<const std::byte> data, const char* label) {
  Buffer buffer(context, data.size_bytes(), Buffer::Access::kGpuOnly, label);
  StagingUploader uploader(context, data.size_bytes());
  uploader.CopyToBuffer(data, buffer.handle(), 0);
  uploader.Finish();
  return buffer;
}

}  // namespace blue::render
