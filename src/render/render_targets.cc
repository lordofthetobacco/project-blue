#include "render/render_targets.h"

#include <algorithm>
#include <cstdint>

#include "render/barriers.h"

namespace blue::render {

namespace {

constexpr VkImageUsageFlags kSampledTarget =
    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

Image MakeTarget(const VulkanContext& context, VkExtent2D extent,
                 VkFormat format, VkImageUsageFlags usage,
                 uint32_t mip_levels = 1) {
  return Image(context, {
                            .extent = extent,
                            .format = format,
                            .usage = usage,
                            .mip_levels = mip_levels,
                            .dedicated = true,
                        });
}

}  // namespace

RenderTargets::RenderTargets(const VulkanContext& context, VkExtent2D extent)
    : context_(context) {
  Resize(extent);
}

VkExtent2D RenderTargets::bloom_mip_extent(uint32_t level) const {
  return {std::max(half_extent_.width >> level, 1u),
          std::max(half_extent_.height >> level, 1u)};
}

void RenderTargets::Resize(VkExtent2D extent) {
  extent_ = extent;
  half_extent_ = {std::max((extent.width + 1) / 2, 1u),
                  std::max((extent.height + 1) / 2, 1u)};

  hdr_ = MakeTarget(context_, extent, kHdrFormat, kSampledTarget);
  depth_ =
      Image(context_, {
                          .extent = extent,
                          .format = kDepthFormat,
                          .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                   VK_IMAGE_USAGE_SAMPLED_BIT,
                          .aspect = VK_IMAGE_ASPECT_DEPTH_BIT,
                          .dedicated = true,
                      });
  ldr_ = MakeTarget(context_, extent, kLdrFormat, kSampledTarget);
  ssao_raw_ =
      MakeTarget(context_, half_extent_, kSsaoRawFormat, kSampledTarget);
  ssao_blur_ =
      MakeTarget(context_, half_extent_, kSsaoRawFormat, kSampledTarget);
  ssao_ = MakeTarget(context_, half_extent_, kSsaoFormat,
                     kSampledTarget | VK_IMAGE_USAGE_TRANSFER_DST_BIT);

  bloom_mip_views_.clear();
  const uint32_t levels =
      std::min(kMaxBloomLevels, MipLevelCount(half_extent_));
  bloom_ =
      MakeTarget(context_, half_extent_, kBloomFormat, kSampledTarget, levels);
  for (uint32_t level = 0; level < levels; ++level) {
    bloom_mip_views_.push_back(
        bloom_.CreateView({.base_mip = level, .mip_count = 1}));
  }
  InitializeLayouts();
}

void RenderTargets::InitializeLayouts() {
  context_.SubmitImmediate([this](VkCommandBuffer cmd) {
    for (const Image* image :
         {&hdr_, &ldr_, &ssao_raw_, &ssao_blur_, &bloom_}) {
      ImageBarrier(cmd, image->handle(), VK_IMAGE_ASPECT_COLOR_BIT, kNoAccess,
                   kFragmentSampledRead, VK_IMAGE_LAYOUT_UNDEFINED,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    constexpr BarrierScope kTransferWrite{VK_PIPELINE_STAGE_2_CLEAR_BIT,
                                          VK_ACCESS_2_TRANSFER_WRITE_BIT};
    ImageBarrier(cmd, ssao_.handle(), VK_IMAGE_ASPECT_COLOR_BIT, kNoAccess,
                 kTransferWrite, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const VkClearColorValue white{{1.0f, 1.0f, 1.0f, 1.0f}};
    const VkImageSubresourceRange range = WholeImage(VK_IMAGE_ASPECT_COLOR_BIT);
    vkCmdClearColorImage(cmd, ssao_.handle(),
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &white, 1,
                         &range);
    ImageBarrier(cmd, ssao_.handle(), VK_IMAGE_ASPECT_COLOR_BIT, kTransferWrite,
                 kFragmentSampledRead, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    ImageBarrier(cmd, depth_.handle(), VK_IMAGE_ASPECT_DEPTH_BIT, kNoAccess,
                 kDepthAttachment, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL);
  });
}

}  // namespace blue::render
