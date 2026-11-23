#ifndef BLUE_RENDER_PASSES_SCENE_PASS_H_
#define BLUE_RENDER_PASSES_SCENE_PASS_H_

#include "render/metal_types.h"

#include "core/extent.h"
#include "render/passes/frame_context.h"

// The HDR scene pass: one render encoder on the HDR color target and the
// scene depth (FrameContext::scene_hdr / scene_depth, over scene_extent from
// the top-left corner: the main view's targets or a scope's picture), inside
// which MeshPass, SkyboxPass, ReticlePass and ScopeLensPass record their
// draws.

namespace blue::render {

// Sets the viewport (Metal's plain one: y down in the framebuffer, y up in
// NDC, as the projection matrices assume, so counter-clockwise stays
// front-facing) and the scissor to cover `extent`. Every camera pass uses it.
void SetSceneViewport(id<MTL4RenderCommandEncoder> encoder,
                      core::Extent2D extent);

// Opens the scene encoder: the HDR target cleared and stored, the depth
// pre-pass result loaded as the depth attachment (and stored, for the
// passes after the scene that test against it); pipelines used inside
// must not write depth. Sets the viewport and scissor (SetSceneViewport())
// and the globals of the context's frame and view (FrameGlobals::Bind()).
// The caller records into the returned encoder and closes it with
// EndScenePass().
[[nodiscard]] id<MTL4RenderCommandEncoder> BeginScenePass(
    const FrameContext& ctx);

// Same, but keeps the HDR target's contents: for draws that must come after
// a pass that rewrites the image (the reflex sight after the weapon motion
// blur). End with EndScenePass().
[[nodiscard]] id<MTL4RenderCommandEncoder> ResumeScenePass(
    const FrameContext& ctx);

// Ends the scene encoder opened by BeginScenePass() or ResumeScenePass().
void EndScenePass(id<MTL4RenderCommandEncoder> encoder);

}  // namespace blue::render

#endif  // BLUE_RENDER_PASSES_SCENE_PASS_H_
