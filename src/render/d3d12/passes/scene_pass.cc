#include "render/d3d12/passes/scene_pass.h"

#include <d3d12.h>

#include "render/d3d12/image.h"
#include "render/d3d12/passes/frame_context.h"
#include "render/d3d12/render_targets.h"

namespace blue::render::d3d12 {

namespace {

// Brings scene_hdr into RENDER_TARGET and the depth into kDepthReadState,
// binds both and sets the viewport and scissor.
void BindHdr(const FrameContext& ctx) {
  ID3D12GraphicsCommandList* cmd = ctx.cmd;
  {
    BarrierBatch barriers(cmd);
    ctx.scene_hdr.Transition(barriers, D3D12_RESOURCE_STATE_RENDER_TARGET);
    ctx.scene_depth.Transition(barriers, RenderTargets::kDepthReadState);
  }
  const D3D12_CPU_DESCRIPTOR_HANDLE rtv = ctx.scene_hdr.rtv();
  const D3D12_CPU_DESCRIPTOR_HANDLE dsv = ctx.scene_depth.read_only_dsv();
  cmd->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
  SetViewport(cmd, ctx.scene_extent);
}

}  // namespace

void BeginScenePass(const FrameContext& ctx) {
  BindHdr(ctx);
  // Only the view's corner, as the Vulkan render area: the rest of a scope
  // picture keeps what it held.
  const D3D12_RECT area{0, 0, static_cast<LONG>(ctx.scene_extent.width),
                        static_cast<LONG>(ctx.scene_extent.height)};
  ctx.cmd->ClearRenderTargetView(ctx.scene_hdr.rtv(), RenderTargets::kHdrClear,
                                 1, &area);
}

void ResumeScenePass(const FrameContext& ctx) { BindHdr(ctx); }

void EndScenePass(const FrameContext& ctx) {
  BarrierBatch barriers(ctx.cmd);
  ctx.scene_hdr.Transition(barriers, ctx.scene_hdr_read_state);
}

}  // namespace blue::render::d3d12
