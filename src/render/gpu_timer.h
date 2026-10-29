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

// Measures GPU time of whole frames using timestamp queries. One query pair
// per frame in flight; results are read back once that frame's fence has
// signaled. Not thread-safe.
class GpuTimer {
 public:
  explicit GpuTimer(const VulkanContext& context);
  ~GpuTimer();

  GpuTimer(const GpuTimer&) = delete;
  GpuTimer& operator=(const GpuTimer&) = delete;

  // Reads back the result of `frame`. Call after the frame's fence was waited
  // on, before recording it again.
  void Collect(size_t frame);
  // Records the start timestamp of `frame` into `cmd`.
  void Begin(VkCommandBuffer cmd, size_t frame);
  // Records the end timestamp of `frame` into `cmd`.
  void End(VkCommandBuffer cmd, size_t frame);

  [[nodiscard]] bool is_supported() const { return pool_ != VK_NULL_HANDLE; }
  // Returns the most recent GPU frame time, or nullopt before the first
  // result or when timestamps are unsupported.
  [[nodiscard]] std::optional<double> last_frame_ms() const {
    return last_frame_ms_;
  }

 private:
  VkDevice device_ = VK_NULL_HANDLE;
  VkQueryPool pool_ = VK_NULL_HANDLE;
  double ms_per_tick_ = 0.0;
  uint64_t valid_mask_ = 0;
  std::array<bool, kFramesInFlight> pending_{};
  std::optional<double> last_frame_ms_;
};

}  // namespace blue::render

#endif  // BLUE_RENDER_GPU_TIMER_H_
