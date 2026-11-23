#ifndef BLUE_RENDER_RENDER_TARGETS_H_
#define BLUE_RENDER_RENDER_TARGETS_H_

#include "render/metal_types.h"

#include <cstdint>
#include <vector>

#include "core/extent.h"
#include "render/image.h"
#include "render/metal_context.h"

namespace blue::render {

// The images a frame renders through, recreated with the swapchain (or the
// editor viewport) and when the upscaling mode changes. There are two sizes:
// the scene is rendered at the RENDER extent and, when the temporal scaler
// is on (temporal()), scaled to the OUTPUT extent, at which the post chain
// runs. Without the scaler both extents are equal and the images that only
// the scaler needs do not exist (empty Images). One set is shared by all
// frames in flight: the single queue executes the frames in order, and the
// barrier every encoder starts with (render/encoders.h) orders the passes
// that write and read these textures. Every image is a render target that
// is also sampled.
//
// At the render extent:
//   depth:     reversed-Z scene depth (Depth32Float) from the depth
//              pre-pass; sampled by SSAO, read-only depth attachment of the
//              scene pass, input of the temporal scaler.
//   ssao_raw,  half of the render extent, RG16Float: r = ambient occlusion,
//   ssao_blur: g = linear view depth (for the depth-aware blur); raw SSAO
//              and its horizontally blurred copy.
//   ssao:      half of the render extent, R8Unorm, the final occlusion
//              (fragment texture kTextureAmbientOcclusion). White when SSAO
//              is off.
//   hdr:       linear scene radiance, RGBA16Float. Color attachment of the
//              scene pass; copied from and blurred in place by the weapon
//              motion blur; input of the temporal scaler.
//   velocity:  RGB10A2Unorm blur vectors of the motion-blurred draw items
//              (shaders/motion_blur.h, EncodeBlur): rg = the vector over
//              twice the largest blur radius, mapped to [0, 1]; a = coverage
//              (1 on those items). 10 bits give a tenth of a pixel at 1080p;
//              the 2-bit alpha is all the coverage needs.
//   motion_tiles: RG16Float, one texel per motion_tile_size() square of
//              pixels: the longest blur vector (pixels) in that tile.
//   motion_source: RGBA16Float copy of the part of `hdr` the motion blur
//              reads while it writes `hdr` (filled by a copy).
//   The last three are only touched in frames with weapon motion blur.
//   motion_vectors: RG16Float, temporal() only: where each pixel's surface
//              was in the previous frame, for the scaler
//              (render/temporal_math.h, MotionVector(): previous minus
//              current position in units of the whole target, x right, y
//              down, without jitter). Written by MotionVectorPass.
//   reactive_mask: R8Unorm, temporal() only: 1 where the scaler must ignore
//              its history (the sights' glass, which has no motion vectors
//              of its own), 0 elsewhere.
// At the output extent:
//   upscaled:  RGBA16Float, temporal() only: the scaler's output, the
//              anti-aliased `hdr` at the output extent.
//   bloom:     RGBA16Float, half of the output extent with bloom_levels()
//              mips: the bloom down/upsample chain; mip 0 ends up holding
//              the sum of every level. A pass renders into mip n with
//              attachment `level` n on bloom().texture() and samples one mip
//              through bloom_mip_view(n).
//   ldr:       the tonemapped image, RGBA8Unorm holding sRGB-encoded colour
//              with luma in alpha (what FXAA reads). Copied to the drawable.
//   selection_mask: R8Unorm coverage of the selected draws, which the
//              overlay pass outlines (editor and focus outline; untouched
//              otherwise).
// post_hdr() is what the post chain (bloom, tonemapping) reads: `upscaled`
// with the scaler, `hdr` without.
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
  static constexpr MTLPixelFormat kMotionVectorFormat = MTLPixelFormatRG16Float;
  static constexpr MTLPixelFormat kReactiveMaskFormat = MTLPixelFormatR8Unorm;
  // Most bloom mips; fewer for tiny windows.
  static constexpr uint32_t kMaxBloomLevels = 6;

  // Creates the targets without the temporal scaler's, at `extent` for both
  // sizes.
  RenderTargets(const MetalContext& context, core::Extent2D extent);

  RenderTargets(const RenderTargets&) = delete;
  RenderTargets& operator=(const RenderTargets&) = delete;

  // Recreates every image for an output of `output` pixels with the scene
  // rendered at `render` pixels, with the temporal scaler's images when
  // `temporal`, and initializes them as described above (blocks for the
  // GPU). Without `temporal` the two extents must be equal. The GPU must no
  // longer use the old images; views handed out before are invalid
  // afterwards.
  void Resize(core::Extent2D output, core::Extent2D render, bool temporal);

  // Whether the temporal scaler's images exist.
  [[nodiscard]] bool temporal() const { return temporal_; }
  [[nodiscard]] core::Extent2D render_extent() const { return render_extent_; }
  [[nodiscard]] core::Extent2D output_extent() const { return output_extent_; }
  // Size of the SSAO images: half of the render extent.
  [[nodiscard]] core::Extent2D ssao_extent() const { return ssao_extent_; }
  // Size of bloom mip 0: half of the output extent.
  [[nodiscard]] core::Extent2D bloom_extent() const { return bloom_extent_; }
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
  // Empty unless temporal().
  [[nodiscard]] const Image& motion_vectors() const { return motion_vectors_; }
  [[nodiscard]] const Image& reactive_mask() const { return reactive_mask_; }
  [[nodiscard]] const Image& upscaled() const { return upscaled_; }
  // The scene radiance at the output extent, which the post chain reads.
  [[nodiscard]] const Image& post_hdr() const {
    return temporal_ ? upscaled_ : hdr_;
  }
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
           motion_tiles_.size_bytes() + motion_source_.size_bytes() +
           motion_vectors_.size_bytes() + reactive_mask_.size_bytes() +
           upscaled_.size_bytes();
  }

 private:
  // Clears ssao_ to white (blocks for the GPU).
  void ClearSsaoToWhite();

  const MetalContext& context_;
  core::Extent2D output_extent_{};
  core::Extent2D render_extent_{};
  core::Extent2D ssao_extent_{};
  core::Extent2D bloom_extent_{};
  bool temporal_ = false;
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
  Image motion_vectors_;
  Image reactive_mask_;
  Image upscaled_;
  int motion_tile_size_ = 1;
  // Views of bloom_'s mips, one per level.
  std::vector<ImageView> bloom_mip_views_;
};

}  // namespace blue::render

#endif  // BLUE_RENDER_RENDER_TARGETS_H_
