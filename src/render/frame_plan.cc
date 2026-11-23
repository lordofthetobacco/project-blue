#include "render/frame_plan.h"

#include "render/motion_blur_math.h"
#include "render/temporal_math.h"

namespace blue::render {

namespace {

// Returns true when the weapon motion blur has something to blur in `view`.
bool WantsMotionBlur(const SceneView& view) {
  const PostSettings& post = view.post;
  if (!post.weapon_motion_blur_enabled ||
      MotionBlurScale(post.weapon_motion_blur_strength, view.frame_seconds) <=
          0.0f) {
    return false;
  }
  for (const DrawItem& item : view.draws) {
    if (item.previous_transform && item.motion_blur) {
      return true;
    }
  }
  return false;
}

}  // namespace

FramePlan MakeFramePlan(const SceneView& view, const BackendCaps& caps) {
  const PostSettings& post = view.post;
  const Upscaling upscaling =
      caps.temporal_scaler ? post.upscaling : Upscaling::kOff;
  const bool temporal = UsesTemporalScaler(upscaling);
  return {
      .shadows = view.shadows_enabled && view.sun.casts_shadows,
      .ssao = post.ssao_enabled,
      .motion_vectors = temporal,
      .temporal = temporal,
      .motion_blur = WantsMotionBlur(view),
      .bloom = post.bloom_enabled,
      .overlay = !view.lines.empty() || !view.overlay_lines.empty() ||
                 !view.selected_draws.empty(),
      .scope = view.scope.has_value(),
      .scope_picture = view.scope.has_value() && view.scope->render_picture,
      .fxaa = post.fxaa_enabled && !temporal,
      .upscaling = upscaling,
  };
}

}  // namespace blue::render
