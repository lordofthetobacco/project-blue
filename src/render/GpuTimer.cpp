#include "render/GpuTimer.hpp"

#include "render/VkCheck.hpp"
#include "render/VulkanContext.hpp"

namespace blue {

namespace {
constexpr uint32_t kQueriesPerFrame = 2;

uint32_t firstQuery(size_t frame)
{
    return static_cast<uint32_t>(frame) * kQueriesPerFrame;
}
} // namespace

GpuTimer::GpuTimer(const VulkanContext& context)
    : device_(context.device())
{
    const uint32_t validBits = context.timestampValidBits();
    const float period = context.properties().limits.timestampPeriod;
    if (validBits == 0 || period <= 0.0f) {
        log::warn("GPU timestamps not supported on this queue, GPU frame time unavailable");
        return;
    }

    validMask_ = validBits >= 64 ? ~0ull : (1ull << validBits) - 1;
    msPerTick_ = static_cast<double>(period) / 1'000'000.0;

    const VkQueryPoolCreateInfo createInfo{
        .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .queryType = VK_QUERY_TYPE_TIMESTAMP,
        .queryCount = static_cast<uint32_t>(kFramesInFlight) * kQueriesPerFrame,
    };
    VK_CHECK(vkCreateQueryPool(device_, &createInfo, nullptr, &pool_));
}

GpuTimer::~GpuTimer()
{
    vkDestroyQueryPool(device_, pool_, nullptr);
}

void GpuTimer::collect(size_t frame)
{
    if (!isSupported() || !pending_[frame]) {
        return;
    }

    std::array<uint64_t, kQueriesPerFrame> ticks{};
    const VkResult result = vkGetQueryPoolResults(device_, pool_, firstQuery(frame), kQueriesPerFrame,
                                                  sizeof(ticks), ticks.data(), sizeof(uint64_t),
                                                  VK_QUERY_RESULT_64_BIT);
    if (result == VK_SUCCESS) {
        const uint64_t elapsed = (ticks[1] - ticks[0]) & validMask_;
        lastFrameMs_ = static_cast<double>(elapsed) * msPerTick_;
        pending_[frame] = false;
    }
}

void GpuTimer::begin(VkCommandBuffer cmd, size_t frame)
{
    if (!isSupported()) {
        return;
    }
    vkCmdResetQueryPool(cmd, pool_, firstQuery(frame), kQueriesPerFrame);
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, pool_, firstQuery(frame));
}

void GpuTimer::end(VkCommandBuffer cmd, size_t frame)
{
    if (!isSupported()) {
        return;
    }
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, pool_, firstQuery(frame) + 1);
    pending_[frame] = true;
}

} // namespace blue
