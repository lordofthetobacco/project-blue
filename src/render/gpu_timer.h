#ifndef BLUE_RENDER_GPU_TIMER_H_
#define BLUE_RENDER_GPU_TIMER_H_

#include "render/metal_types.h"

#include <array>
#include <cstddef>
#include <optional>

#include "render/gpu_scope.h"

namespace blue::render {

// Measures the GPU time of a frame and of named scopes in it. The frame total
// comes from the GPUStartTime / GPUEndTime of the frame's command buffer,
// read once it has completed. Per-scope times will come from counter samples
// attached to the encoders a scope opens (render/encoders.h); until then
// every scope but kTotal reads nullopt. Not thread-safe.
class GpuTimer {
 public:
  static constexpr size_t kScopeCount = kGpuScopeCount;

  GpuTimer() = default;

  GpuTimer(const GpuTimer&) = delete;
  GpuTimer& operator=(const GpuTimer&) = delete;

  // Reads the times of the frame recorded into `command_buffer`, which must
  // have completed. Call after waiting for a frame slot's previous command
  // buffer, before recording into the slot again.
  void Collect(id<MTLCommandBuffer> command_buffer);

  // Starts / ends `scope` in the frame being recorded: the encoders opened in
  // between count towards it. Every BeginScope() needs a matching EndScope()
  // in the same frame.
  void BeginScope(GpuScope scope);
  void EndScope(GpuScope scope);
  // Called by render/encoders.h for every encoder of a frame before it is
  // created: attaches the sample buffer of the current scope.
  void AttachSamples(MTLRenderPassDescriptor* descriptor);
  void AttachSamples(MTLBlitPassDescriptor* descriptor);

  // Returns the most recent GPU time of `scope`, or nullopt before the first
  // result, when the scope did not run in the last collected frame, or when
  // it cannot be measured.
  [[nodiscard]] std::optional<double> last_ms(GpuScope scope) const {
    return last_ms_[static_cast<size_t>(scope)];
  }
  // Returns the most recent GPU frame time; see last_ms().
  [[nodiscard]] std::optional<double> last_frame_ms() const {
    return last_ms(GpuScope::kTotal);
  }

 private:
  std::array<std::optional<double>, kScopeCount> last_ms_;
};

}  // namespace blue::render

#endif  // BLUE_RENDER_GPU_TIMER_H_
