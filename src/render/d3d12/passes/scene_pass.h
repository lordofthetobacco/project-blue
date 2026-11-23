#ifndef BLUE_RENDER_D3D12_PASSES_SCENE_PASS_H_
#define BLUE_RENDER_D3D12_PASSES_SCENE_PASS_H_

#include "render/d3d12/passes/frame_context.h"

namespace blue::render::d3d12 {

// Brings the HDR target into RENDER_TARGET, clears it to
// RenderTargets::kHdrClear and binds it with the depth pre-pass result as a
// read-only depth (read_only_dsv(), the depth in kDepthReadState): every
// pipeline used inside must not write depth. Sets the viewport and scissor.
void BeginScenePass(const FrameContext& ctx);
// Leaves the HDR target in PIXEL_SHADER_RESOURCE for the post chain; the
// depth target stays in RenderTargets::kDepthReadState.
void EndScenePass(const FrameContext& ctx);

}  // namespace blue::render::d3d12

#endif  // BLUE_RENDER_D3D12_PASSES_SCENE_PASS_H_
