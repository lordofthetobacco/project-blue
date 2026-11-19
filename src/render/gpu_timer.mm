#include "render/gpu_timer.h"

#include <optional>

namespace blue::render {

namespace {

constexpr double kMsPerSecond = 1000.0;

}  // namespace

void GpuTimer::Collect(id<MTLCommandBuffer> command_buffer) {
  if (command_buffer == nil) {
    return;  // Nothing was recorded into this frame slot yet.
  }
  last_ms_.fill(std::nullopt);
  // Both times are 0 for a command buffer that failed or never ran.
  const CFTimeInterval start = command_buffer.GPUStartTime;
  const CFTimeInterval end = command_buffer.GPUEndTime;
  if (command_buffer.status == MTLCommandBufferStatusCompleted && end > start) {
    last_ms_[static_cast<size_t>(GpuScope::kTotal)] =
        (end - start) * kMsPerSecond;
  }
}

// TODO(metal-stage5): per-scope times from MTLCounterSampleBuffer
// stage-boundary samples: BeginScope()/EndScope() pick the scope, the
// AttachSamples() overloads attach its sample buffer, Collect() resolves it.
void GpuTimer::BeginScope(GpuScope /*scope*/) {}

void GpuTimer::EndScope(GpuScope /*scope*/) {}

void GpuTimer::AttachSamples(MTLRenderPassDescriptor* /*descriptor*/) {}

void GpuTimer::AttachSamples(MTLBlitPassDescriptor* /*descriptor*/) {}

}  // namespace blue::render
