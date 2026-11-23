#ifndef BLUE_RENDER_D3D12_GPU_TIMER_H_
#define BLUE_RENDER_D3D12_GPU_TIMER_H_

#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <cstddef>
#include <optional>

#include "render/d3d12/buffer.h"
#include "render/frame_config.h"
#include "render/gpu_scope.h"

namespace blue::render::d3d12 {

class D3d12Context;

// Measures GPU time of named scopes with timestamp queries: one query pair
// per scope and frame in flight in a timestamp query heap, resolved at the
// end of the frame (EndFrame()) into a readback buffer that is read once
// that frame's fence was reached (Collect()). The same interface and
// semantics as the Vulkan backend's GpuTimer. Not thread-safe.
class GpuTimer {
 public:
  static constexpr size_t kScopeCount = kGpuScopeCount;

  explicit GpuTimer(const D3d12Context& context);

  GpuTimer(const GpuTimer&) = delete;
  GpuTimer& operator=(const GpuTimer&) = delete;

  // Reads back the results of `frame`. Call after the frame's fence was
  // reached, before recording it again.
  void Collect(size_t frame);
  // Records the start / end timestamp of `scope` of `frame` into `cmd`.
  // Every Begin() needs a matching End() in the same frame.
  void Begin(ID3D12GraphicsCommandList* cmd, size_t frame, GpuScope scope);
  void End(ID3D12GraphicsCommandList* cmd, size_t frame, GpuScope scope);
  // Records the resolve of `frame`'s queries; after its last End().
  void EndFrame(ID3D12GraphicsCommandList* cmd, size_t frame);

  // Returns the most recent GPU time of `scope`, or nullopt before the first
  // result or when the scope did not run in the last collected frame.
  [[nodiscard]] std::optional<double> last_ms(GpuScope scope) const {
    return last_ms_[static_cast<size_t>(scope)];
  }
  [[nodiscard]] std::optional<double> last_frame_ms() const {
    return last_ms(GpuScope::kTotal);
  }

 private:
  Microsoft::WRL::ComPtr<ID3D12QueryHeap> heap_;
  Buffer readback_;
  double ms_per_tick_ = 0.0;
  // A scope is pending from its End() until its result is collected.
  std::array<std::array<bool, kScopeCount>, kFramesInFlight> pending_{};
  std::array<std::optional<double>, kScopeCount> last_ms_;
};

}  // namespace blue::render::d3d12

#endif  // BLUE_RENDER_D3D12_GPU_TIMER_H_
