#ifndef BLUE_RENDER_PASSES_SCENE_PASS_H_
#define BLUE_RENDER_PASSES_SCENE_PASS_H_

#include "render/metal_types.h"

#include "core/extent.h"
#include "render/passes/frame_context.h"

// The HDR scene pass: one render encoder on the HDR color target and the
// scene depth, inside which MeshPass, SkyboxPass and ReticlePass record
// their draws.

namespace blue::render {

// Sets the viewport (Metal's plain one: y down in the framebuffer, y up in
// NDC, as the projection matrices assume, so counter-clockwise stays
// front-facing) and the scissor to cover `extent`. Every camera pass uses it.
void SetSceneViewport(id<MTLRenderCommandEncoder> encoder,
                      core::Extent2D extent);

// Opens the scene encoder: the HDR target cleared and stored, the depth
// pre-pass result loaded as the depth attachment (and stored, for the
// passes after the scene that test against it); pipelines used inside
// must not write depth. Sets the viewport and scissor (SetSceneViewport())
// and the frame's globals (FrameGlobals::Bind()). The caller records into
// the returned encoder and closes it with EndScenePass().
[[nodiscard]] id<MTLRenderCommandEncoder> BeginScenePass(
    const FrameContext& ctx);

// Ends the scene encoder opened by BeginScenePass().
void EndScenePass(id<MTLRenderCommandEncoder> encoder);

}  // namespace blue::render

#endif  // BLUE_RENDER_PASSES_SCENE_PASS_H_
