#ifndef BLUE_RENDER_GPU_TIMER_H_
#define BLUE_RENDER_GPU_TIMER_H_

#include "render/metal_types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "render/frame_config.h"
#include "render/gpu_scope.h"
#include "render/metal_context.h"

namespace blue::render {

// Measures the GPU time of a frame and of named scopes in it.
//
// The frame total (GpuScope::kTotal) comes from the GPUStartTime /
// GPUEndTime of the frame's command buffer. A scope's time comes from
// timestamp counter samples that every encoder opened while the scope is
// open (render/encoders.h) takes at its start and end (a render encoder at
// the start and end of its fragment stage): the scope spans from its
// encoders' earliest start to their latest end. Every result is read once
// the frame's command buffer has completed.
//
// On a tile-based GPU (every Apple GPU) consecutive encoders overlap in
// time, e.g. the vertex stage of one encoder runs during the fragment stage
// of the previous one, and independent passes may run side by side, so the
// per-scope numbers are approximate and need not add up to the total.
//
// Per-scope times need stage-boundary counter sampling with the common
// timestamp counter set; without it every scope but kTotal reads nullopt.
// Not thread-safe.
class GpuTimer {
 public:
  static constexpr size_t kScopeCount = kGpuScopeCount;
  // Encoders per frame that get timestamps; the ones a frame opens beyond
  // this go unmeasured.
  static constexpr size_t kMaxEncodersPerFrame = 64;

  explicit GpuTimer(const MetalContext& context);

  GpuTimer(const GpuTimer&) = delete;
  GpuTimer& operator=(const GpuTimer&) = delete;

  // Reads the times of the frame last recorded into frame slot `frame`
  // ([0, kFramesInFlight)) from its `command_buffer`, which must have
  // completed (nil when the slot has not recorded yet), and makes `frame`
  // the slot the next encoders record into. Call after waiting for the
  // slot's previous command buffer, before recording into it again.
  void Collect(size_t frame, id<MTLCommandBuffer> command_buffer);

  // Starts / ends `scope` in the frame being recorded: the encoders opened in
  // between count towards it. Scopes do not nest. Every BeginScope() needs a
  // matching EndScope() in the same frame.
  void BeginScope(GpuScope scope);
  void EndScope(GpuScope scope);
  // Called by render/encoders.h for every encoder of a frame before it is
  // created: attaches the sample buffer when a scope is open.
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
  // The encoders a frame slot has recorded with samples, in order: encoder
  // i owns samples 2 i (start) and 2 i + 1 (end) of the slot's range.
  struct FrameSamples {
    std::array<GpuScope, kMaxEncodersPerFrame> scopes{};
    size_t encoder_count = 0;
  };

  // Reserves the next sample pair of the frame being recorded for an
  // encoder of the open scope; returns its first sample index, or nullopt
  // when nothing is to be sampled (no scope open, no counter support, or
  // every pair of the frame is taken).
  [[nodiscard]] std::optional<NSUInteger> ReserveSamples();
  // Fills last_ms_ for every scope but kTotal from the samples of `frame`.
  void ResolveScopes(size_t frame);

  // nil when per-scope timing is unsupported.
  id<MTLCounterSampleBuffer> samples_ = nil;
  std::array<FrameSamples, kFramesInFlight> frames_{};
  size_t frame_ = 0;
  std::optional<GpuScope> open_scope_;
  bool overflow_logged_ = false;
  std::array<std::optional<double>, kScopeCount> last_ms_;
};

}  // namespace blue::render

#endif  // BLUE_RENDER_GPU_TIMER_H_
