#ifndef BLUE_RENDER_METAL_GPU_TIMER_H_
#define BLUE_RENDER_METAL_GPU_TIMER_H_

#include "render/metal/metal_types.h"

#include <array>
#include <cstddef>
#include <optional>

#include "render/frame_config.h"
#include "render/gpu_scope.h"
#include "render/metal/metal_context.h"

namespace blue::render::metal {

// Measures the GPU time of a frame and of named scopes in it.
//
// The frame total (GpuScope::kTotal) is the GPU time the commit feedback of
// the frame's command buffer reports (Submission::gpu_seconds()). A scope's
// time is the difference of two timestamps the command buffer writes into
// an MTL4CounterHeap, one where the scope begins and one where it ends
// (-[MTL4CommandBuffer writeTimestampIntoHeap:atIndex:]): each is taken when
// all work recorded before it has completed. Every result is read once the
// frame's work has completed.
//
// Every encoder waits for the output of the ones before it (the barrier of
// render/encoders.h), so a scope's time is close to the time its own
// encoders ran; work the GPU starts early for a later scope (the vertex
// stage of its encoders) is counted in the scope during which it runs, so
// the numbers are approximate and need not add up to the total.
//
// Without a timestamp counter heap every scope but kTotal reads nullopt.
// Not thread-safe.
class GpuTimer {
 public:
  static constexpr size_t kScopeCount = kGpuScopeCount;

  explicit GpuTimer(const MetalContext& context);

  GpuTimer(const GpuTimer&) = delete;
  GpuTimer& operator=(const GpuTimer&) = delete;

  // Reads the times of the frame last recorded into frame slot `frame`
  // ([0, kFramesInFlight)) from `submission`, its completed commit (null
  // when the slot has not recorded yet), and makes `frame` the slot the
  // next scopes record into. Call after waiting for the slot's previous
  // work, before recording into it again.
  void Collect(size_t frame, const Submission* submission);

  // Starts / ends `scope` in the frame being recorded on `cmd`, between
  // encoders (none may be open): the encoders opened in between count
  // towards it. Scopes do not nest. Every BeginScope() needs a matching
  // EndScope() in the same frame.
  void BeginScope(id<MTL4CommandBuffer> cmd, GpuScope scope);
  void EndScope(id<MTL4CommandBuffer> cmd, GpuScope scope);

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
  // Returns the heap index of the begin timestamp of `scope` in frame slot
  // `frame`; its end timestamp is the next entry.
  [[nodiscard]] static NSUInteger EntryIndex(size_t frame, GpuScope scope);
  // Fills last_ms_ for every scope but kTotal from the timestamps of
  // `frame`.
  void ResolveScopes(size_t frame);

  // nil when per-scope timing is unsupported.
  id<MTL4CounterHeap> heap_ = nil;
  // Ticks of a heap timestamp per second.
  double ticks_per_second_ = 0.0;
  // The scopes each frame slot recorded.
  std::array<std::array<bool, kScopeCount>, kFramesInFlight> recorded_{};
  size_t frame_ = 0;
  std::optional<GpuScope> open_scope_;
  std::array<std::optional<double>, kScopeCount> last_ms_;
};

}  // namespace blue::render::metal

#endif  // BLUE_RENDER_METAL_GPU_TIMER_H_
