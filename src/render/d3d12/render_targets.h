#ifndef BLUE_RENDER_D3D12_RENDER_TARGETS_H_
#define BLUE_RENDER_D3D12_RENDER_TARGETS_H_

#include <dxgiformat.h>

#include <cstdint>

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
// Between frames hdr and ldr are PIXEL_SHADER_RESOURCE and depth is
// DEPTH_READ | PIXEL_SHADER_RESOURCE. Created through a clear (placed render
// targets must be initialized before use). Borrows `context`. Neither
// copyable nor movable.
class RenderTargets {
 public:
  static constexpr DXGI_FORMAT kHdrFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
  static constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D32_FLOAT;
  static constexpr DXGI_FORMAT kLdrFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
  // Linear radiance where nothing (not even the sky) is drawn: what the
  // scene pass clears hdr to (its optimized clear value).
  static constexpr float kHdrClear[4] = {0.015f, 0.018f, 0.022f, 1.0f};
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
  [[nodiscard]] uint64_t size_bytes() const {
    return hdr_.size_bytes() + depth_.size_bytes() + ldr_.size_bytes();
  }

 private:
  const D3d12Context& context_;
  core::Extent2D extent_{};
  Image hdr_;
  Image depth_;
  Image ldr_;
};

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
