#include "render/metal_context.h"

#include <algorithm>

#include "core/log.h"
#include "platform/window.h"
#include "render/metal_check.h"

namespace blue::render {

MetalContext::MetalContext(const platform::Window& /*window*/) {
  device_ = MTLCreateSystemDefaultDevice();
  if (device_ == nil) {
    core::log::Fatal("No Metal device available");
  }
  device_name_ = device_.name.UTF8String;
  if (![device_ supportsFamily:MTLGPUFamilyMetal3] ||
      !device_.supportsBCTextureCompression) {
    core::log::Fatal(
        "GPU {} does not support Metal 3 with BC texture compression "
        "(supportsFamily:MTLGPUFamilyMetal3, supportsBCTextureCompression)",
        device_name_);
  }

  queue_ = [device_ newCommandQueue];
  FatalIfNil(queue_, nil, "newCommandQueue");
  queue_.label = @"Main queue";

  core::log::Info("GPU: {} (Metal 3, {} memory, {} MiB working set)",
                  device_name_,
                  device_.hasUnifiedMemory ? "unified" : "discrete",
                  device_.recommendedMaxWorkingSetSize >> 20);
}

void MetalContext::SubmitImmediate(
    const std::function<void(id<MTLCommandBuffer>)>& record) const {
  // Drains the autoreleased objects of the recording (encoders, descriptors)
  // right away instead of whenever the caller's pool ends.
  @autoreleasepool {
    const id<MTLCommandBuffer> command_buffer = [queue_ commandBuffer];
    FatalIfNil(command_buffer, nil, "commandBuffer");
    command_buffer.label = @"Immediate";
    record(command_buffer);
    [command_buffer commit];
    [command_buffer waitUntilCompleted];
    CheckCommandBuffer(command_buffer);
  }
}

void MetalContext::Commit(id<MTLCommandBuffer> command_buffer) const {
  [command_buffer commit];
  last_committed_ = command_buffer;
}

void MetalContext::WaitIdle() const {
  [last_committed_ waitUntilCompleted];
  // Lets the completed command buffer (and what it references, such as a
  // drawable) go.
  last_committed_ = nil;
}

void MetalContext::AddStagingBytes(uint64_t bytes) const {
  staging_usage_.current += bytes;
  staging_usage_.peak = std::max(staging_usage_.peak, staging_usage_.current);
}

void MetalContext::RemoveStagingBytes(uint64_t bytes) const {
  staging_usage_.current -= std::min(bytes, staging_usage_.current);
}

}  // namespace blue::render
