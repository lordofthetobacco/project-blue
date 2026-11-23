#ifndef BLUE_RENDER_D3D12_RENDER_TARGETS_H_
#define BLUE_RENDER_D3D12_RENDER_TARGETS_H_

#include <dxgiformat.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include "core/extent.h"
#include "render/d3d12/image.h"

namespace blue::render::d3d12 {

class D3d12Context;

// The images sized like the scene output (the window or the editor
// viewport), with the formats of the Vulkan backend's RenderTargets:
//   hdr    R16G16B16A16_FLOAT  the scene's linear radiance
//   depth  D32_FLOAT           reversed-Z scene depth (R32_TYPELESS resource,
//                              R32_FLOAT SRV for the passes that sample it)
//   ldr    R8G8B8A8_UNORM      tonemapped, sRGB-encoded colour, luma in alpha
//   ssao_raw, ssao_blur  half resolution R16G16_FLOAT: occlusion and linear
//                        view depth (for the depth-aware blur)
//   ssao   half resolution R8_UNORM, the final occlusion (FrameGlobals t4;
//          white = none)
//   bloom  bloom_levels() R16G16B16A16_FLOAT images, level i at half
//          resolution >> i: the down/upsample chain; level 0 ends up holding
//          the sum. One image per level rather than mips of one image:
//          GPU-based validation reports reading any single-mip view while
//          mip 0 of the same resource is a render target (the last upsample)
//          as a hazard, so the levels are separate resources.
//   selection_mask  R8_UNORM coverage of the selected draws, which the
//                   overlay pass outlines (editor only)
//   velocity  R10G10B10A2_UNORM blur vectors of the motion-blurred draw
//             items (shaders/motion_blur.slang, EncodeBlur; the bit layout
//             of Vulkan's A2B10G10R10_UNORM_PACK32)
//   motion_tiles   R16G16_FLOAT, one texel per motion_tile_size() square of
//                  pixels: the longest blur vector in it
//   motion_source  R16G16B16A16_FLOAT copy of the part of hdr the motion
//                  blur reads while it writes hdr (not a render target)
// Between frames every target is PIXEL_SHADER_RESOURCE (every bloom level) but
// depth, which is DEPTH_READ | PIXEL_SHADER_RESOURCE. Created through a
// clear or discard (placed render targets must be initialized before use).
// Borrows `context`. Neither copyable nor movable.
class RenderTargets {
 public:
  static constexpr DXGI_FORMAT kHdrFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
  static constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D32_FLOAT;
  static constexpr DXGI_FORMAT kLdrFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
  static constexpr DXGI_FORMAT kSsaoRawFormat = DXGI_FORMAT_R16G16_FLOAT;
  static constexpr DXGI_FORMAT kSsaoFormat = DXGI_FORMAT_R8_UNORM;
  static constexpr DXGI_FORMAT kBloomFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
  static constexpr DXGI_FORMAT kSelectionMaskFormat = DXGI_FORMAT_R8_UNORM;
  static constexpr DXGI_FORMAT kVelocityFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
  static constexpr DXGI_FORMAT kMotionTileFormat = DXGI_FORMAT_R16G16_FLOAT;
  // Most bloom mips; fewer for tiny outputs.
  static constexpr uint32_t kMaxBloomLevels = 6;
  // Linear radiance where nothing (not even the sky) is drawn: what the
  // scene pass clears hdr to (its optimized clear value).
  static constexpr float kHdrClear[4] = {0.015f, 0.018f, 0.022f, 1.0f};
  // "No motion" in the velocity target: a zero blur vector (0.5 in the
  // encoding) and no coverage (shaders/motion_blur.slang, EncodeBlur).
  static constexpr float kNoMotion[4] = {0.5f, 0.5f, 0.0f, 0.0f};
  // What the overlay pass clears the selection mask to: not selected.
  static constexpr float kNotSelected[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  // The state of the depth target between passes that only read it.
  static constexpr D3D12_RESOURCE_STATES kDepthReadState =
      D3D12_RESOURCE_STATE_DEPTH_READ |
      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

  RenderTargets(const D3d12Context& context, core::Extent2D extent);

  RenderTargets(const RenderTargets&) = delete;
  RenderTargets& operator=(const RenderTargets&) = delete;

  // Recreates every target at `extent`; no frame in flight may use them.
  void Resize(core::Extent2D extent);

  [[nodiscard]] core::Extent2D extent() const { return extent_; }
  [[nodiscard]] Image& hdr() { return hdr_; }
  [[nodiscard]] Image& depth() { return depth_; }
  [[nodiscard]] Image& ldr() { return ldr_; }
  [[nodiscard]] const Image& ldr() const { return ldr_; }
  // The size of the half-resolution images (SSAO, bloom mip 0).
  [[nodiscard]] core::Extent2D half_extent() const { return half_extent_; }
  [[nodiscard]] Image& ssao_raw() { return ssao_raw_; }
  [[nodiscard]] Image& ssao_blur() { return ssao_blur_; }
  [[nodiscard]] Image& ssao() { return ssao_; }
  // Bloom level `level` (< bloom_levels()).
  [[nodiscard]] Image& bloom(uint32_t level) { return bloom_[level]; }
  [[nodiscard]] uint32_t bloom_levels() const {
    return static_cast<uint32_t>(bloom_.size());
  }
  [[nodiscard]] core::Extent2D bloom_mip_extent(uint32_t level) const {
    return {std::max(half_extent_.width >> level, 1u),
            std::max(half_extent_.height >> level, 1u)};
  }
  [[nodiscard]] Image& selection_mask() { return selection_mask_; }
  [[nodiscard]] Image& velocity() { return velocity_; }
  [[nodiscard]] Image& motion_tiles() { return motion_tiles_; }
  [[nodiscard]] Image& motion_source() { return motion_source_; }
  // Pixels per side of a motion_tiles() texel: the largest blur radius,
  // MotionBlurMaxRadius() of the height.
  [[nodiscard]] int motion_tile_size() const { return motion_tile_size_; }
  [[nodiscard]] uint64_t size_bytes() const {
    return hdr_.size_bytes() + depth_.size_bytes() + ldr_.size_bytes() +
           ssao_raw_.size_bytes() + ssao_blur_.size_bytes() +
           ssao_.size_bytes() + bloom_bytes() + selection_mask_.size_bytes() +
           velocity_.size_bytes() + motion_tiles_.size_bytes() +
           motion_source_.size_bytes();
  }

 private:
  [[nodiscard]] uint64_t bloom_bytes() const {
    uint64_t bytes = 0;
    for (const Image& level : bloom_) {
      bytes += level.size_bytes();
    }
    return bytes;
  }

  const D3d12Context& context_;
  core::Extent2D extent_{};
  core::Extent2D half_extent_{};
  Image hdr_;
  Image depth_;
  Image ldr_;
  Image ssao_raw_;
  Image ssao_blur_;
  Image ssao_;
  std::vector<Image> bloom_;
  Image selection_mask_;
  Image velocity_;
  Image motion_tiles_;
  Image motion_source_;
  int motion_tile_size_ = 1;
};

// Returns a depth target of `extent` in kDepthFormat (an R32_TYPELESS
// resource with a DSV, a read-only DSV and an R32_FLOAT staging SRV),
// cleared to reversed-Z infinity and left in kDepthReadState; call between
// frames.
[[nodiscard]] Image MakeDepthTarget(const D3d12Context& context,
                                    core::Extent2D extent, const wchar_t* name);

// Returns a render target of `extent` in `format` (views in `format`, a
// staging SRV, and with `shader_visible` one for ImGui), cleared to `clear`
// (its optimized clear value) and left in `state`. The clear is submitted
// here and waited for: call between frames.
[[nodiscard]] Image MakeColorTarget(const D3d12Context& context,
                                    core::Extent2D extent, DXGI_FORMAT format,
                                    D3D12_RESOURCE_STATES state,
                                    const float (&clear)[4],
                                    bool shader_visible, const wchar_t* name);

}  // namespace blue::render::d3d12

#endif  // BLUE_RENDER_D3D12_RENDER_TARGETS_H_
