#include "render/d3d12/render_targets.h"

#include <d3d12.h>

#include <algorithm>

#include "core/extent.h"
#include "render/d3d12/d3d12_context.h"
#include "render/d3d12/image.h"

namespace blue::render::d3d12 {

namespace {

constexpr float kBlack[4] = {0.0f, 0.0f, 0.0f, 1.0f};
// Reversed-Z: infinity is 0 (DepthPrepass clears to it).
constexpr float kDepthClear = 0.0f;

Image MakeDepthTarget(const D3d12Context& context, core::Extent2D extent) {
  D3D12_CLEAR_VALUE clear{};
  clear.Format = RenderTargets::kDepthFormat;
  clear.DepthStencil = {.Depth = kDepthClear, .Stencil = 0};
  Image image(context, {.extent = extent,
                        .format = DXGI_FORMAT_R32_TYPELESS,
                        .flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
                        .initial_state = D3D12_RESOURCE_STATE_DEPTH_WRITE,
                        .clear_value = clear,
                        .name = L"blue scene depth"});
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

}  // namespace

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
  depth_ = MakeDepthTarget(context_, extent_);
  ldr_ = MakeColorTarget(context_, extent_, kLdrFormat,
                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, kBlack,
                         false, L"blue ldr");
}

}  // namespace blue::render::d3d12
