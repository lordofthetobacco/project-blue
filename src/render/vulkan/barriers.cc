#include "render/vulkan/barriers.h"

namespace blue::render::vulkan {

void ImageBarrier(VkCommandBuffer cmd, VkImage image,
                  const VkImageSubresourceRange& range, BarrierScope src,
                  BarrierScope dst, VkImageLayout old_layout,
                  VkImageLayout new_layout) {
  const VkImageMemoryBarrier2 barrier{
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
      .srcStageMask = src.stage,
      .srcAccessMask = src.access,
      .dstStageMask = dst.stage,
      .dstAccessMask = dst.access,
      .oldLayout = old_layout,
      .newLayout = new_layout,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = image,
      .subresourceRange = range,
  };
  const VkDependencyInfo dependency{
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .imageMemoryBarrierCount = 1,
      .pImageMemoryBarriers = &barrier,
  };
  vkCmdPipelineBarrier2(cmd, &dependency);
}

void ImageBarrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect,
                  BarrierScope src, BarrierScope dst, VkImageLayout old_layout,
                  VkImageLayout new_layout) {
  ImageBarrier(cmd, image, WholeImage(aspect), src, dst, old_layout,
               new_layout);
}

}  // namespace blue::render::vulkan
