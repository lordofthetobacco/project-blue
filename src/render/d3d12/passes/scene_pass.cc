#include "render/d3d12/passes/scene_pass.h"

#include <d3d12.h>

#include "render/d3d12/image.h"
#include "render/d3d12/passes/frame_context.h"
#include "render/d3d12/render_targets.h"

namespace blue::render::d3d12 {

void BeginScenePass(const FrameContext& ctx) {
  ID3D12GraphicsCommandList* cmd = ctx.cmd;
  Image& hdr = ctx.targets.hdr();
  {
    BarrierBatch barriers(cmd);
    hdr.Transition(barriers, D3D12_RESOURCE_STATE_RENDER_TARGET);
    ctx.targets.depth().Transition(barriers, RenderTargets::kDepthReadState);
  }
  const D3D12_CPU_DESCRIPTOR_HANDLE rtv = hdr.rtv();
  const D3D12_CPU_DESCRIPTOR_HANDLE dsv = ctx.targets.depth().read_only_dsv();
  cmd->ClearRenderTargetView(rtv, RenderTargets::kHdrClear, 0, nullptr);
  cmd->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
  SetViewport(cmd, ctx.targets.extent());
}

void EndScenePass(const FrameContext& ctx) {
  BarrierBatch barriers(ctx.cmd);
  ctx.targets.hdr().Transition(barriers,
                               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

}  // namespace blue::render::d3d12
