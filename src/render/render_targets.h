#ifndef BLUE_RENDER_RENDER_TARGETS_H_
#define BLUE_RENDER_RENDER_TARGETS_H_

#include "render/metal_types.h"

#include <cstdint>
#include <vector>

#include "core/extent.h"
#include "render/image.h"
#include "render/metal_context.h"

namespace blue::render {

// The window-sized images a frame renders through, recreated with the
// swapchain. One set is shared by all frames in flight: the single queue
// executes the frames in order, and the barrier every encoder starts with
// (render/encoders.h) orders the passes that write and read these textures. Every image
// is a render target that is also sampled.
//
//   depth:     reversed-Z scene depth (Depth32Float) from the depth
//              pre-pass; sampled by SSAO, read-only depth attachment of the
//              scene pass.
//   ssao_raw,  half resolution RG16Float: r = ambient occlusion, g = linear
//   ssao_blur: view depth (for the depth-aware blur); raw SSAO and its
//              horizontally blurred copy.
//   ssao:      half resolution R8Unorm, the final occlusion (fragment
//              texture kTextureAmbientOcclusion). White when SSAO is off.
//   hdr:       linear scene radiance, RGBA16Float. Color attachment of the
//              scene pass, sampled by bloom and tonemapping; copied from and
//              blurred in place by the weapon motion blur.
//   bloom:     RGBA16Float, half resolution with bloom_levels() mips: the
//              bloom down/upsample chain; mip 0 ends up holding the sum of
//              every level. A pass renders into mip n with attachment
//              `level` n on bloom().texture() and samples one mip through
//              bloom_mip_view(n).
//   ldr:       the tonemapped image, RGBA8Unorm holding sRGB-encoded colour
//              with luma in alpha (what FXAA reads). Copied to the drawable.
//   selection_mask: R8Unorm coverage of the selected draws, which the
//              overlay pass outlines (editor only; untouched otherwise).
//   velocity:  RGB10A2Unorm blur vectors of the motion-blurred draw items
//              (shaders/motion_blur.h, EncodeBlur): rg = the vector over
//              twice the largest blur radius, mapped to [0, 1]; a = coverage
//              (1 on those items). 10 bits give a tenth of a pixel at 1080p;
//              the 2-bit alpha is all the coverage needs.
//   motion_tiles: RG16Float, one texel per motion_tile_size() square of
//              pixels: the longest blur vector (pixels) in that tile.
//   motion_source: RGBA16Float copy of the part of `hdr` the motion blur
//              reads while it writes `hdr` (filled by a blit).
//   The last three are only touched in frames with weapon motion blur.
//
// After creation (and every Resize) every image has undefined contents,
// except `ssao`, which is white.
// Neither copyable nor movable. Not thread-safe.
class RenderTargets {
 public:
  static constexpr MTLPixelFormat kHdrFormat = MTLPixelFormatRGBA16Float;
  static constexpr MTLPixelFormat kDepthFormat = MTLPixelFormatDepth32Float;
  static constexpr MTLPixelFormat kLdrFormat = MTLPixelFormatRGBA8Unorm;
  static constexpr MTLPixelFormat kSsaoRawFormat = MTLPixelFormatRG16Float;
  static constexpr MTLPixelFormat kSsaoFormat = MTLPixelFormatR8Unorm;
  static constexpr MTLPixelFormat kBloomFormat = MTLPixelFormatRGBA16Float;
  static constexpr MTLPixelFormat kSelectionMaskFormat = MTLPixelFormatR8Unorm;
  // Red in the low 10 bits, alpha in the top 2.
  static constexpr MTLPixelFormat kVelocityFormat = MTLPixelFormatRGB10A2Unorm;
  static constexpr MTLPixelFormat kMotionTileFormat = MTLPixelFormatRG16Float;
  // Most bloom mips; fewer for tiny windows.
  static constexpr uint32_t kMaxBloomLevels = 6;

  RenderTargets(const MetalContext& context, core::Extent2D extent);

  RenderTargets(const RenderTargets&) = delete;
  RenderTargets& operator=(const RenderTargets&) = delete;

  // Recreates every image at `extent` and initializes them as described
  // above (blocks for the GPU). The GPU must no longer use the old ones;
  // views handed out before are invalid afterwards.
  void Resize(core::Extent2D extent);

  [[nodiscard]] core::Extent2D extent() const { return extent_; }
  // Size of the half-resolution images (SSAO, bloom mip 0).
  [[nodiscard]] core::Extent2D half_extent() const { return half_extent_; }
  [[nodiscard]] const Image& hdr() const { return hdr_; }
  [[nodiscard]] const Image& depth() const { return depth_; }
  [[nodiscard]] const Image& ldr() const { return ldr_; }
  [[nodiscard]] const Image& ssao_raw() const { return ssao_raw_; }
  [[nodiscard]] const Image& ssao_blur() const { return ssao_blur_; }
  [[nodiscard]] const Image& ssao() const { return ssao_; }
  [[nodiscard]] const Image& bloom() const { return bloom_; }
  [[nodiscard]] const Image& selection_mask() const { return selection_mask_; }
  [[nodiscard]] const Image& velocity() const { return velocity_; }
  [[nodiscard]] const Image& motion_tiles() const { return motion_tiles_; }
  [[nodiscard]] const Image& motion_source() const { return motion_source_; }
  // Pixels per side of a motion_tiles() texel: the largest blur radius,
  // MotionBlurMaxRadius() of the height.
  [[nodiscard]] int motion_tile_size() const { return motion_tile_size_; }
  [[nodiscard]] uint32_t bloom_levels() const { return bloom_.mip_levels(); }
  // A texture of bloom mip `level` alone (< bloom_levels()), for sampling
  // that mip; render into it with attachment `level` on bloom().texture().
  [[nodiscard]] id<MTLTexture> bloom_mip_view(uint32_t level) const {
    return bloom_mip_views_[level].handle();
  }
  // Returns the extent of bloom mip `level`.
  [[nodiscard]] core::Extent2D bloom_mip_extent(uint32_t level) const;
  // GPU memory of all the targets, in bytes.
  [[nodiscard]] uint64_t size_bytes() const {
    return hdr_.size_bytes() + depth_.size_bytes() + ldr_.size_bytes() +
           ssao_raw_.size_bytes() + ssao_blur_.size_bytes() +
           ssao_.size_bytes() + bloom_.size_bytes() +
           selection_mask_.size_bytes() + velocity_.size_bytes() +
           motion_tiles_.size_bytes() + motion_source_.size_bytes();
  }

 private:
  // Clears ssao_ to white (blocks for the GPU).
  void ClearSsaoToWhite();

  const MetalContext& context_;
  core::Extent2D extent_{};
  core::Extent2D half_extent_{};
  Image hdr_;
  Image depth_;
  Image ldr_;
  Image ssao_raw_;
  Image ssao_blur_;
  Image ssao_;
  Image bloom_;
  Image selection_mask_;
  Image velocity_;
  Image motion_tiles_;
  Image motion_source_;
  int motion_tile_size_ = 1;
  // Views of bloom_'s mips, one per level.
  std::vector<ImageView> bloom_mip_views_;
};

}  // namespace blue::render

#endif  // BLUE_RENDER_RENDER_TARGETS_H_
