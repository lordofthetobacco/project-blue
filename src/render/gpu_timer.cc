#include "render/gpu_timer.h"

#include "core/log.h"
#include "render/vk_check.h"

namespace blue::render {

namespace {

constexpr uint32_t kQueriesPerFrame = 2;

uint32_t FirstQuery(size_t frame) {
  return static_cast<uint32_t>(frame) * kQueriesPerFrame;
}

}  // namespace

GpuTimer::GpuTimer(const VulkanContext& context) : device_(context.device()) {
  const uint32_t valid_bits = context.timestamp_valid_bits();
  const float period = context.properties().limits.timestampPeriod;
  if (valid_bits == 0 || period <= 0.0f) {
    core::log::Warn(
        "GPU timestamps not supported on this queue, GPU frame time "
        "unavailable");
    return;
  }

  valid_mask_ =
      valid_bits >= 64 ? ~uint64_t{0} : (uint64_t{1} << valid_bits) - 1;
  ms_per_tick_ = static_cast<double>(period) / 1'000'000.0;

  const VkQueryPoolCreateInfo create_info{
      .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
      .queryType = VK_QUERY_TYPE_TIMESTAMP,
      .queryCount = static_cast<uint32_t>(kFramesInFlight) * kQueriesPerFrame,
  };
  CheckVk(vkCreateQueryPool(device_, &create_info, nullptr, &pool_));
}

GpuTimer::~GpuTimer() { vkDestroyQueryPool(device_, pool_, nullptr); }

void GpuTimer::Collect(size_t frame) {
  if (!is_supported() || !pending_[frame]) {
    return;
  }

  std::array<uint64_t, kQueriesPerFrame> ticks{};
  const VkResult result = vkGetQueryPoolResults(
      device_, pool_, FirstQuery(frame), kQueriesPerFrame, sizeof(ticks),
      ticks.data(), sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
  if (result == VK_SUCCESS) {
    const uint64_t elapsed = (ticks[1] - ticks[0]) & valid_mask_;
    last_frame_ms_ = static_cast<double>(elapsed) * ms_per_tick_;
    pending_[frame] = false;
  }
}

void GpuTimer::Begin(VkCommandBuffer cmd, size_t frame) {
  if (!is_supported()) {
    return;
  }
  vkCmdResetQueryPool(cmd, pool_, FirstQuery(frame), kQueriesPerFrame);
  vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, pool_,
                       FirstQuery(frame));
}

void GpuTimer::End(VkCommandBuffer cmd, size_t frame) {
  if (!is_supported()) {
    return;
  }
  vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, pool_,
                       FirstQuery(frame) + 1);
  pending_[frame] = true;
}

}  // namespace blue::render
