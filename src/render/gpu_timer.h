#ifndef BLUE_RENDER_GPU_TIMER_H_
#define BLUE_RENDER_GPU_TIMER_H_

#include <vulkan/vulkan.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "render/frame_config.h"
#include "render/vulkan_context.h"

namespace blue::render {

// A named span of GPU work measured by GpuTimer. To time a new pass, add an
// enumerator before kCount and its name to GpuScopeName(); the overlay lists
// every scope.
enum class GpuScope {
  kTotal,         // The whole frame.
  kShadow,        // The shadow cascades; absent in frames that skip them.
  kDepthPrepass,  // Scene depth of the camera view.
  kSsao,          // Ambient occlusion and its blur; absent when off.
  kScene,         // The HDR scene pass: meshes, sky and the reflex sight.
  kBloom,         // Downsample / upsample chain; absent when off.
  kTonemap,       // Bloom mix, exposure and tonemapping into the LDR image.
  kPresent,       // LDR image to the swapchain, with FXAA when on.
  kUi,            // The ImGui overlay.
  kCount,         // Number of scopes; not a scope.
};

inline constexpr size_t kGpuScopeCount = static_cast<size_t>(GpuScope::kCount);

// Returns the display name of `scope`, e.g. "shadow".
[[nodiscard]] const char* GpuScopeName(GpuScope scope);

// Measures GPU time of named scopes using timestamp queries. One query pair
// per scope and frame in flight; results are read back once that frame's
// fence has signaled. Not thread-safe.
class GpuTimer {
 public:
  static constexpr size_t kScopeCount = kGpuScopeCount;

  explicit GpuTimer(const VulkanContext& context);
  ~GpuTimer();

  GpuTimer(const GpuTimer&) = delete;
  GpuTimer& operator=(const GpuTimer&) = delete;

  // Reads back the results of `frame`. Call after the frame's fence was waited
  // on, before recording it again.
  void Collect(size_t frame);
  // Resets the queries of `frame`; must be recorded before any Begin() or
  // End() of that frame, outside of a rendering pass.
  void BeginFrame(VkCommandBuffer cmd, size_t frame);
  // Records the start timestamp of `scope` of `frame` into `cmd`.
  void Begin(VkCommandBuffer cmd, size_t frame, GpuScope scope);
  // Records the end timestamp of `scope` of `frame` into `cmd`. Every Begin()
  // needs a matching End() in the same frame.
  void End(VkCommandBuffer cmd, size_t frame, GpuScope scope);

  [[nodiscard]] bool is_supported() const { return pool_ != VK_NULL_HANDLE; }
  // Returns the most recent GPU time of `scope`, or nullopt before the first
  // result, when the scope did not run in the last collected frame, or when
  // timestamps are unsupported.
  [[nodiscard]] std::optional<double> last_ms(GpuScope scope) const {
    return last_ms_[static_cast<size_t>(scope)];
  }
  // Returns the most recent GPU frame time; see last_ms().
  [[nodiscard]] std::optional<double> last_frame_ms() const {
    return last_ms(GpuScope::kTotal);
  }

 private:
  VkDevice device_ = VK_NULL_HANDLE;
  VkQueryPool pool_ = VK_NULL_HANDLE;
  double ms_per_tick_ = 0.0;
  uint64_t valid_mask_ = 0;
  // A scope is pending from its End() until its result is collected.
  std::array<std::array<bool, kScopeCount>, kFramesInFlight> pending_{};
  std::array<std::optional<double>, kScopeCount> last_ms_;
};

}  // namespace blue::render

#endif  // BLUE_RENDER_GPU_TIMER_H_
