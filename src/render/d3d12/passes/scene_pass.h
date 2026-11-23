#ifndef BLUE_RENDER_D3D12_PASSES_SCENE_PASS_H_
#define BLUE_RENDER_D3D12_PASSES_SCENE_PASS_H_

#include "render/d3d12/passes/frame_context.h"

// The HDR scene pass: FrameContext::scene_hdr bound with the read-only
// scene_depth over scene_extent from the top-left corner (the main view's
// targets or a scope's picture), into which MeshPass, SkyboxPass,
// ReticlePass and ScopeLensPass record their draws.

namespace blue::render::d3d12 {

// Brings scene_hdr into RENDER_TARGET, clears scene_extent of it to
// RenderTargets::kHdrClear and binds it with the depth pre-pass result as a
// read-only depth (read_only_dsv(), the depth in kDepthReadState): every
// pipeline used inside must not write depth. Sets the viewport and scissor.
void BeginScenePass(const FrameContext& ctx);

// Same, but keeps scene_hdr's contents: for draws that must come after a
// pass that rewrites the image (the sights after the weapon motion blur).
// End with EndScenePass().
void ResumeScenePass(const FrameContext& ctx);

// Leaves scene_hdr in ctx.scene_hdr_read_state for whoever samples it; the
// depth stays in RenderTargets::kDepthReadState.
void EndScenePass(const FrameContext& ctx);

}  // namespace blue::render::d3d12

#endif  // BLUE_RENDER_D3D12_PASSES_SCENE_PASS_H_
