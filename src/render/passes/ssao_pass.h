#ifndef BLUE_RENDER_PASSES_SSAO_PASS_H_
#define BLUE_RENDER_PASSES_SSAO_PASS_H_

#include "render/metal_types.h"

#include <string>

#include "render/fullscreen_pass.h"
#include "render/metal_context.h"
#include "render/passes/frame_context.h"
#include "render/render_targets.h"
#include "render/samplers.h"
#include "render/shader_programs.h"

namespace blue::render {

// Screen-space ambient occlusion at half resolution
// (shaders/ssao.frag.metal, reading the scene depth), then a separable
// depth-aware blur (shaders/ssao_blur.frag.metal) into the R8 target that
// FrameGlobals binds at kTextureAmbientOcclusion. Settings come from
// SceneView::post (ssao_enabled, ssao_radius, ssao_intensity). When SSAO is
// off the pass costs nothing beyond one clear of the target to white when
// it is switched off. Borrows `context` and `programs`, which must outlive
// it. Neither copyable nor movable. Not thread-safe.
class SsaoPass {
 public:
  // Most of the screen height the sampling radius may cover, so geometry a
  // few centimetres from the eye (the weapon) gets a small kernel instead of
  // a screen-wide one.
  static constexpr float kMaxRadiusScreenFraction = 0.1f;

  SsaoPass(const MetalContext& context, ShaderPrograms& programs,
           const std::string& shader_dir);

  SsaoPass(const SsaoPass&) = delete;
  SsaoPass& operator=(const SsaoPass&) = delete;

  // Points the pass at the current targets; call after they were created or
  // resized (which leaves the AO target white).
  void BindTargets(const RenderTargets& targets, const Samplers& samplers);

  // Records the occlusion and the blur, three render encoders, after the
  // depth pre-pass. With SSAO off it records nothing, except one clear to
  // white right after it was switched off.
  void Record(const FrameContext& ctx);

 private:
  // Records the occlusion and blur passes.
  void RecordOcclusion(const FrameContext& ctx) const;
  // Records a clear of the AO target to white (no occlusion).
  static void RecordClearToWhite(const FrameContext& ctx);

  FullscreenPass occlusion_;        // depth -> ssao_raw (RG16F)
  FullscreenPass blur_horizontal_;  // ssao_raw -> ssao_blur (RG16F)
  FullscreenPass blur_vertical_;    // ssao_blur -> ssao (R8)
  // True while the AO target holds white, so turning SSAO off clears it
  // once and then records nothing.
  bool target_white_ = true;
};

}  // namespace blue::render

#endif  // BLUE_RENDER_PASSES_SSAO_PASS_H_
