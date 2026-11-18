#ifndef BLUE_RENDER_PASSES_SSAO_PASS_H_
#define BLUE_RENDER_PASSES_SSAO_PASS_H_

#include <string>

#include "render/fullscreen_pass.h"
#include "render/passes/frame_context.h"
#include "render/render_targets.h"
#include "render/samplers.h"
#include "render/shader_programs.h"
#include "render/vulkan_context.h"

namespace blue::render {

// Screen-space ambient occlusion at half resolution (shaders/ssao.frag), then
// a separable depth-aware blur (shaders/ssao_blur.frag) into the R8 target
// that FrameGlobals binds as binding 4. Settings come from
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

  SsaoPass(const VulkanContext& context, ShaderPrograms& programs,
           const std::string& shader_dir);

  SsaoPass(const SsaoPass&) = delete;
  SsaoPass& operator=(const SsaoPass&) = delete;

  // Points the pass at the current targets; call after they were created or
  // resized (which leaves the AO target white), while the GPU is idle.
  void BindTargets(const RenderTargets& targets, const Samplers& samplers);

  // Expects the depth target in DEPTH_READ_ONLY_OPTIMAL (DepthPrepass leaves
  // it there). Leaves every SSAO target in SHADER_READ_ONLY_OPTIMAL. Must be
  // recorded outside of a rendering pass. With SSAO off it records nothing,
  // except one clear to white right after it was switched off.
  void Record(const FrameContext& ctx);

 private:
  // Records the occlusion and blur passes.
  void RecordOcclusion(const FrameContext& ctx) const;
  // Records a clear of the AO target to white (no occlusion).
  static void RecordClearToWhite(const FrameContext& ctx);

  FullscreenPass occlusion_;
  FullscreenPass blur_horizontal_;  // ssao_raw -> ssao_blur (RG16F)
  FullscreenPass blur_vertical_;    // ssao_blur -> ssao (R8)
  // True while the AO target holds white, so turning SSAO off clears it
  // once and then records nothing.
  bool target_white_ = true;
};

}  // namespace blue::render

#endif  // BLUE_RENDER_PASSES_SSAO_PASS_H_
