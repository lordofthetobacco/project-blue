#ifndef BLUE_RENDER_RENDER_TARGETS_H_
#define BLUE_RENDER_RENDER_TARGETS_H_

#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

#include "render/image.h"
#include "render/vulkan_context.h"

namespace blue::render {

// The window-sized images a frame renders through, recreated with the
// swapchain. One set is shared by all frames in flight: each pass's first
// barrier on an image waits for the previous frame's last access to it (on
// the single queue that orders the frames), see the passes.
//
//   depth:     reversed-Z scene depth (D32) from the depth pre-pass; sampled
//              by SSAO, read-only depth attachment of the scene pass.
//   ssao_raw,  half resolution RG16F: r = ambient occlusion, g = linear view
//   ssao_blur: depth (for the depth-aware blur); raw SSAO and its
//              horizontally blurred copy.
//   ssao:      half resolution R8, the final occlusion (FrameGlobals binding
//              4). White when SSAO is off.
//   hdr:       linear scene radiance, R16G16B16A16_SFLOAT. Color attachment
//              of the scene pass, sampled by bloom and tonemapping.
//   bloom:     R16G16B16A16_SFLOAT, half resolution with bloom_levels() mips:
//              the bloom down/upsample chain; mip 0 ends up holding the sum
//              of every level.
//   ldr:       the tonemapped image, R8G8B8A8_UNORM holding sRGB-encoded
//              colour with luma in alpha (what FXAA reads). Copied to the
//              swapchain.
//   selection_mask: R8_UNORM coverage of the selected draws, which the
//              overlay pass outlines (editor only; untouched otherwise).
//
// After creation (and every Resize) all sampled images are in
// SHADER_READ_ONLY_OPTIMAL with undefined contents, except `ssao` (white)
// and `depth` (DEPTH_READ_ONLY_OPTIMAL, undefined contents).
// Neither copyable nor movable. Not thread-safe.
class RenderTargets {
 public:
  static constexpr VkFormat kHdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
  static constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
  static constexpr VkFormat kLdrFormat = VK_FORMAT_R8G8B8A8_UNORM;
  static constexpr VkFormat kSsaoRawFormat = VK_FORMAT_R16G16_SFLOAT;
  static constexpr VkFormat kSsaoFormat = VK_FORMAT_R8_UNORM;
  static constexpr VkFormat kBloomFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
  static constexpr VkFormat kSelectionMaskFormat = VK_FORMAT_R8_UNORM;
  // Most bloom mips; fewer for tiny windows.
  static constexpr uint32_t kMaxBloomLevels = 6;

  RenderTargets(const VulkanContext& context, VkExtent2D extent);

  RenderTargets(const RenderTargets&) = delete;
  RenderTargets& operator=(const RenderTargets&) = delete;

  // Recreates every image at `extent` and initializes them as described
  // above (blocks for the GPU). The GPU must no longer use the old ones;
  // views handed out before are invalid afterwards.
  void Resize(VkExtent2D extent);

  [[nodiscard]] VkExtent2D extent() const { return extent_; }
  // Size of the half-resolution images (SSAO, bloom mip 0).
  [[nodiscard]] VkExtent2D half_extent() const { return half_extent_; }
  [[nodiscard]] const Image& hdr() const { return hdr_; }
  [[nodiscard]] const Image& depth() const { return depth_; }
  [[nodiscard]] const Image& ldr() const { return ldr_; }
  [[nodiscard]] const Image& ssao_raw() const { return ssao_raw_; }
  [[nodiscard]] const Image& ssao_blur() const { return ssao_blur_; }
  [[nodiscard]] const Image& ssao() const { return ssao_; }
  [[nodiscard]] const Image& bloom() const { return bloom_; }
  [[nodiscard]] const Image& selection_mask() const { return selection_mask_; }
  [[nodiscard]] uint32_t bloom_levels() const { return bloom_.mip_levels(); }
  // The view of bloom mip `level` alone (< bloom_levels()).
  [[nodiscard]] VkImageView bloom_mip_view(uint32_t level) const {
    return bloom_mip_views_[level].handle();
  }
  // Returns the extent of bloom mip `level`.
  [[nodiscard]] VkExtent2D bloom_mip_extent(uint32_t level) const;

 private:
  // Moves every image into the layout documented above.
  void InitializeLayouts();

  const VulkanContext& context_;
  VkExtent2D extent_{};
  VkExtent2D half_extent_{};
  Image hdr_;
  Image depth_;
  Image ldr_;
  Image ssao_raw_;
  Image ssao_blur_;
  Image ssao_;
  Image bloom_;
  Image selection_mask_;
  // Destroyed before bloom_ is replaced (Resize clears them first).
  std::vector<ImageView> bloom_mip_views_;
};

}  // namespace blue::render

#endif  // BLUE_RENDER_RENDER_TARGETS_H_
