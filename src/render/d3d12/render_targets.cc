#include "render/d3d12/render_targets.h"

#include <d3d12.h>

#include <algorithm>

#include "core/extent.h"
#include "render/d3d12/d3d12_context.h"
#include "render/d3d12/image.h"
#include "render/motion_blur_math.h"
#include "render/texture_types.h"

namespace blue::render::d3d12 {

namespace {

constexpr float kBlack[4] = {0.0f, 0.0f, 0.0f, 1.0f};
constexpr float kWhite[4] = {1.0f, 1.0f, 1.0f, 1.0f};
// Reversed-Z: infinity is 0 (DepthPrepass clears to it).
constexpr float kDepthClear = 0.0f;

}  // namespace

Image MakeDepthTarget(const D3d12Context& context, core::Extent2D extent,
                      const wchar_t* name) {
  D3D12_CLEAR_VALUE clear{};
  clear.Format = RenderTargets::kDepthFormat;
  clear.DepthStencil = {.Depth = kDepthClear, .Stencil = 0};
  Image image(context, {.extent = extent,
                        .format = DXGI_FORMAT_R32_TYPELESS,
                        .flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
                        .initial_state = D3D12_RESOURCE_STATE_DEPTH_WRITE,
                        .clear_value = clear,
                        .name = name});
  image.CreateDsv(RenderTargets::kDepthFormat);
  image.CreateSrv(DXGI_FORMAT_R32_FLOAT, /*shader_visible=*/false);
  context.SubmitImmediate([&](ID3D12GraphicsCommandList* cmd) {
    cmd->ClearDepthStencilView(image.dsv(), D3D12_CLEAR_FLAG_DEPTH, kDepthClear,
                               0, 0, nullptr);
    BarrierBatch barriers(cmd);
    image.Transition(barriers, RenderTargets::kDepthReadState);
  });
  return image;
}

Image MakeColorTarget(const D3d12Context& context, core::Extent2D extent,
                      DXGI_FORMAT format, D3D12_RESOURCE_STATES state,
                      const float (&clear)[4], bool shader_visible,
                      const wchar_t* name) {
  D3D12_CLEAR_VALUE clear_value{};
  clear_value.Format = format;
  std::copy(std::begin(clear), std::end(clear), clear_value.Color);
  Image image(context, {.extent = extent,
                        .format = format,
                        .flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                        .initial_state = D3D12_RESOURCE_STATE_RENDER_TARGET,
                        .clear_value = clear_value,
                        .name = name});
  image.AddRtv(format);
  image.CreateSrv(format, shader_visible);
  context.SubmitImmediate([&](ID3D12GraphicsCommandList* cmd) {
    cmd->ClearRenderTargetView(image.rtv(), clear, 0, nullptr);
    BarrierBatch barriers(cmd);
    image.Transition(barriers, state);
  });
  return image;
}

RenderTargets::RenderTargets(const D3d12Context& context, core::Extent2D extent)
    : context_(context) {
  Resize(extent);
}

void RenderTargets::Resize(core::Extent2D extent) {
  extent_ = {std::max(extent.width, 1u), std::max(extent.height, 1u)};
  hdr_ = MakeColorTarget(context_, extent_, kHdrFormat,
                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, kHdrClear,
                         false, L"blue hdr");
  depth_ = MakeDepthTarget(context_, extent_, L"blue scene depth");
  ldr_ = MakeColorTarget(context_, extent_, kLdrFormat,
                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, kBlack,
                         false, L"blue ldr");
  half_extent_ = {std::max((extent_.width + 1) / 2, 1u),
                  std::max((extent_.height + 1) / 2, 1u)};
  constexpr D3D12_RESOURCE_STATES kRead =
      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  ssao_raw_ = MakeColorTarget(context_, half_extent_, kSsaoRawFormat, kRead,
                              kBlack, false, L"blue ssao raw");
  ssao_blur_ = MakeColorTarget(context_, half_extent_, kSsaoRawFormat, kRead,
                               kBlack, false, L"blue ssao blur");
  // White: no occlusion until SSAO runs.
  ssao_ = MakeColorTarget(context_, half_extent_, kSsaoFormat, kRead, kWhite,
                          false, L"blue ssao");
  const uint32_t levels =
      std::min(kMaxBloomLevels, MipLevelCount(half_extent_));
  bloom_.clear();
  for (uint32_t level = 0; level < levels; ++level) {
    bloom_.push_back(MakeColorTarget(context_, bloom_mip_extent(level),
                                     kBloomFormat, kRead, kBlack, false,
                                     L"blue bloom"));
  }

  selection_mask_ =
      MakeColorTarget(context_, extent_, kSelectionMaskFormat, kRead,
                      kNotSelected, false, L"blue selection mask");
  motion_tile_size_ = MotionBlurMaxRadius(extent_.height);
  const auto tile_size = static_cast<uint32_t>(motion_tile_size_);
  velocity_ = MakeColorTarget(context_, extent_, kVelocityFormat, kRead,
                              kNoMotion, false, L"blue velocity");
  motion_tiles_ = MakeColorTarget(
      context_,
      {(extent_.width + tile_size - 1) / tile_size,
       (extent_.height + tile_size - 1) / tile_size},
      kMotionTileFormat, kRead, kBlack, false, L"blue motion tiles");
  // Only copied into and sampled: no render target, no initialization.
  motion_source_ = Image(context_, {.extent = extent_,
                                    .format = kHdrFormat,
                                    .initial_state = kRead,
                                    .name = L"blue motion source"});
  motion_source_.CreateSrv(kHdrFormat, /*shader_visible=*/false);
}

}  // namespace blue::render::d3d12
