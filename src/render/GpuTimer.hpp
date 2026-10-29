#pragma once

#include "render/FrameConfig.hpp"

#include <vulkan/vulkan.h>

#include <array>
#include <optional>

namespace blue {

class VulkanContext;

// Measures GPU time of whole frames using timestamp queries. One query pair per
// frame in flight; results are read back once that frame's fence has signaled.
class GpuTimer {
public:
    explicit GpuTimer(const VulkanContext& context);
    ~GpuTimer();

    GpuTimer(const GpuTimer&) = delete;
    GpuTimer& operator=(const GpuTimer&) = delete;

    // Call after the frame's fence was waited on, before recording it again.
    void collect(size_t frame);
    void begin(VkCommandBuffer cmd, size_t frame);
    void end(VkCommandBuffer cmd, size_t frame);

    [[nodiscard]] bool isSupported() const { return pool_ != VK_NULL_HANDLE; }
    [[nodiscard]] std::optional<double> lastFrameMs() const { return lastFrameMs_; }

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueryPool pool_ = VK_NULL_HANDLE;
    double msPerTick_ = 0.0;
    uint64_t validMask_ = 0;
    std::array<bool, kFramesInFlight> pending_{};
    std::optional<double> lastFrameMs_;
};

} // namespace blue
