#ifndef BLUE_RENDER_BARRIERS_H_
#define BLUE_RENDER_BARRIERS_H_

#include <vulkan/vulkan.h>

#include <cstdint>

// Image layout transitions and the synchronization scopes the render passes
// use most, so every pass spells its barriers the same way.

namespace blue::render {

// Source or destination synchronization scope of a barrier.
struct BarrierScope {
  VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_NONE;
  VkAccessFlags2 access = VK_ACCESS_2_NONE;
};

// Nothing to wait for / nothing waits (e.g. the old contents are discarded).
inline constexpr BarrierScope kNoAccess{};
// Written as a color attachment (also the stage where a swapchain image's
// acquire semaphore is waited on).
inline constexpr BarrierScope kColorAttachmentWrite{
    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT};
// Loaded and written as a color attachment (blending, LOAD_OP_LOAD).
inline constexpr BarrierScope kColorAttachmentReadWrite{
    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
    VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT};
// Depth tested and written.
inline constexpr BarrierScope kDepthAttachment{
    VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
        VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
// Sampled by fragment shaders.
inline constexpr BarrierScope kFragmentSampledRead{
    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT};

// Returns the range covering every mip level and layer of `aspect`.
[[nodiscard]] constexpr VkImageSubresourceRange WholeImage(
    VkImageAspectFlags aspect) {
  return {aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
}

// Records one image memory barrier that moves `range` of `image` from
// `old_layout` to `new_layout`, making the `src` work visible to `dst`.
// VK_IMAGE_LAYOUT_UNDEFINED as `old_layout` discards the contents.
void ImageBarrier(VkCommandBuffer cmd, VkImage image,
                  const VkImageSubresourceRange& range, BarrierScope src,
                  BarrierScope dst, VkImageLayout old_layout,
                  VkImageLayout new_layout);

// Same as above for every mip level and layer of `aspect`.
void ImageBarrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect,
                  BarrierScope src, BarrierScope dst, VkImageLayout old_layout,
                  VkImageLayout new_layout);

}  // namespace blue::render

#endif  // BLUE_RENDER_BARRIERS_H_
