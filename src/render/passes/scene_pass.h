#ifndef BLUE_RENDER_PASSES_SCENE_PASS_H_
#define BLUE_RENDER_PASSES_SCENE_PASS_H_

#include "render/passes/frame_context.h"

// The HDR scene pass: one dynamic-rendering scope on the HDR color target
// and the scene depth (FrameContext::scene_hdr / scene_depth, over
// scene_extent from the top-left corner: the main view's targets or a
// scope's picture), inside which MeshPass, SkyboxPass, ReticlePass and
// ScopeLensPass record their draws.

namespace blue::render {

// Moves the HDR target into the attachment layout (discarding its
// contents) and begins rendering into it, cleared, with the depth pre-pass
// result as a read-only depth attachment (DEPTH_READ_ONLY_OPTIMAL, loaded):
// every pipeline used inside must not write depth. Sets the Y-up viewport
// (negative height, so clip space matches GLM and CCW stays front-facing)
// and scissor.
void BeginScenePass(const FrameContext& ctx);

// Same, but keeps the HDR target's contents: for draws that must come after
// a pass that rewrites the image (the reflex sight after the weapon motion
// blur). Expects the HDR target in SHADER_READ_ONLY_OPTIMAL, last written as
// a color attachment. End with EndScenePass().
void ResumeScenePass(const FrameContext& ctx);

// Ends the rendering scope. Leaves the HDR target in
// SHADER_READ_ONLY_OPTIMAL for the post chain; the depth target stays
// DEPTH_READ_ONLY_OPTIMAL.
void EndScenePass(const FrameContext& ctx);

}  // namespace blue::render

#endif  // BLUE_RENDER_PASSES_SCENE_PASS_H_
