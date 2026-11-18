#include "render/gpu_timer.h"

#include "core/log.h"
#include "render/vk_check.h"

namespace blue::render {

namespace {

constexpr uint32_t kQueriesPerScope = 2;
constexpr uint32_t kQueriesPerFrame =
    static_cast<uint32_t>(GpuTimer::kScopeCount) * kQueriesPerScope;

uint32_t FirstFrameQuery(size_t frame) {
  return static_cast<uint32_t>(frame) * kQueriesPerFrame;
}

uint32_t FirstScopeQuery(size_t frame, GpuScope scope) {
  return FirstFrameQuery(frame) +
         static_cast<uint32_t>(scope) * kQueriesPerScope;
}

}  // namespace

const char* GpuScopeName(GpuScope scope) {
  switch (scope) {
    case GpuScope::kTotal:
      return "frame";
    case GpuScope::kShadow:
      return "shadow";
    case GpuScope::kDepthPrepass:
      return "depth";
    case GpuScope::kSsao:
      return "ssao";
    case GpuScope::kScene:
      return "scene";
    case GpuScope::kBloom:
      return "bloom";
    case GpuScope::kTonemap:
      return "tonemap";
    case GpuScope::kPresent:
      return "present";
    case GpuScope::kUi:
      return "ui";
    case GpuScope::kCount:
      break;
  }
  return "?";
}

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
  if (!is_supported()) {
    return;
  }
  std::array<bool, kScopeCount>& pending = pending_[frame];
  if (!pending[static_cast<size_t>(GpuScope::kTotal)]) {
    return;  // The frame was never recorded, or already collected.
  }

  for (size_t scope = 0; scope < kScopeCount; ++scope) {
    if (!pending[scope]) {
      last_ms_[scope] = std::nullopt;  // The scope did not run in this frame.
      continue;
    }
    std::array<uint64_t, kQueriesPerScope> ticks{};
    const VkResult result = vkGetQueryPoolResults(
        device_, pool_, FirstScopeQuery(frame, static_cast<GpuScope>(scope)),
        kQueriesPerScope, sizeof(ticks), ticks.data(), sizeof(uint64_t),
        VK_QUERY_RESULT_64_BIT);
    if (result == VK_SUCCESS) {
      const uint64_t elapsed = (ticks[1] - ticks[0]) & valid_mask_;
      last_ms_[scope] = static_cast<double>(elapsed) * ms_per_tick_;
      pending[scope] = false;
    }
  }
}

void GpuTimer::BeginFrame(VkCommandBuffer cmd, size_t frame) {
  if (!is_supported()) {
    return;
  }
  vkCmdResetQueryPool(cmd, pool_, FirstFrameQuery(frame), kQueriesPerFrame);
}

void GpuTimer::Begin(VkCommandBuffer cmd, size_t frame, GpuScope scope) {
  if (!is_supported()) {
    return;
  }
  vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, pool_,
                       FirstScopeQuery(frame, scope));
}

void GpuTimer::End(VkCommandBuffer cmd, size_t frame, GpuScope scope) {
  if (!is_supported()) {
    return;
  }
  vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, pool_,
                       FirstScopeQuery(frame, scope) + 1);
  pending_[frame][static_cast<size_t>(scope)] = true;
}

}  // namespace blue::render
