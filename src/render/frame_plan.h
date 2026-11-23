#ifndef BLUE_RENDER_FRAME_PLAN_H_
#define BLUE_RENDER_FRAME_PLAN_H_

#include "render/scene_view.h"

namespace blue::render {

// What a backend can do beyond the passes every backend has
// (Renderer::caps()).
struct BackendCaps {
  // A temporal scaler runs PostSettings::upscaling; without one every mode
  // renders as Upscaling::kOff.
  bool temporal_scaler = false;
};

// Which passes a frame runs: the decisions every backend makes the same way
// from the SceneView, so that all backends skip the same work. A skipped
// pass costs nothing (no GPU time scope, no draw).
struct FramePlan {
  bool shadows = false;         // Sun shadow cascades (shadows on, sun casts).
  bool ssao = false;            // Ambient occlusion and its blur.
  bool motion_vectors = false;  // Scene motion vectors for the scaler.
  bool temporal = false;        // The temporal scaler (upscaling != kOff).
  // Weapon motion blur: enabled, a positive blur scale, and at least one item
  // with a previous_transform whose motion_blur is set. The backend still
  // skips it when none of those items is visible.
  bool motion_blur = false;
  bool bloom = false;
  bool overlay = false;        // Debug lines or a selection outline exist.
  bool scope = false;          // SceneView::scope is set.
  bool scope_picture = false;  // ... and its picture is rendered this frame.
  bool fxaa = false;           // FXAA in the present pass (not with temporal).
  // PostSettings::upscaling, or kOff without a temporal scaler.
  Upscaling upscaling = Upscaling::kOff;
};

// Returns the plan of `view` on a backend with `caps`. Allocates nothing.
[[nodiscard]] FramePlan MakeFramePlan(const SceneView& view,
                                      const BackendCaps& caps);

}  // namespace blue::render

#endif  // BLUE_RENDER_FRAME_PLAN_H_
