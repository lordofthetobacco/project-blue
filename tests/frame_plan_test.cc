#include "render/frame_plan.h"

#include <doctest/doctest.h>

#include <array>

#include <glm/mat4x4.hpp>

#include "render/debug_lines.h"
#include "render/scene_view.h"

namespace blue::render {

namespace {

constexpr BackendCaps kNoScaler{};
constexpr BackendCaps kScaler{.temporal_scaler = true};

// A view with every effect on, the sun casting shadows and a 60 Hz frame.
SceneView MakeView() {
  SceneView view;
  view.sun.casts_shadows = true;
  view.shadows_enabled = true;
  view.frame_seconds = 1.0f / 60.0f;
  return view;
}

}  // namespace

TEST_CASE("The plan follows the scene's switches") {
  SceneView view = MakeView();
  FramePlan plan = MakeFramePlan(view, kNoScaler);
  CHECK(plan.shadows);
  CHECK(plan.ssao == view.post.ssao_enabled);
  CHECK(plan.bloom == view.post.bloom_enabled);
  CHECK(plan.fxaa == view.post.fxaa_enabled);
  CHECK_FALSE(plan.overlay);
  CHECK_FALSE(plan.scope);
  CHECK_FALSE(plan.scope_picture);
  CHECK_FALSE(plan.motion_blur);

  view.shadows_enabled = false;
  CHECK_FALSE(MakeFramePlan(view, kNoScaler).shadows);
  view.shadows_enabled = true;
  view.sun.casts_shadows = false;
  CHECK_FALSE(MakeFramePlan(view, kNoScaler).shadows);

  view.post.ssao_enabled = false;
  view.post.bloom_enabled = false;
  view.post.fxaa_enabled = false;
  plan = MakeFramePlan(view, kNoScaler);
  CHECK_FALSE(plan.ssao);
  CHECK_FALSE(plan.bloom);
  CHECK_FALSE(plan.fxaa);
}

TEST_CASE("Upscaling needs a temporal scaler") {
  SceneView view = MakeView();
  view.post.upscaling = Upscaling::kQuality;
  view.post.fxaa_enabled = true;

  const FramePlan without = MakeFramePlan(view, kNoScaler);
  CHECK(without.upscaling == Upscaling::kOff);
  CHECK_FALSE(without.temporal);
  CHECK_FALSE(without.motion_vectors);
  CHECK(without.fxaa);  // The scaler that would replace it does not exist.

  const FramePlan with = MakeFramePlan(view, kScaler);
  CHECK(with.upscaling == Upscaling::kQuality);
  CHECK(with.temporal);
  CHECK(with.motion_vectors);
  CHECK_FALSE(with.fxaa);

  view.post.upscaling = Upscaling::kOff;
  const FramePlan off = MakeFramePlan(view, kScaler);
  CHECK_FALSE(off.temporal);
  CHECK(off.fxaa);
}

TEST_CASE("Weapon motion blur needs a blurred item that moved") {
  SceneView view = MakeView();
  std::array<DrawItem, 2> draws{};
  view.draws = draws;
  CHECK_FALSE(MakeFramePlan(view, kNoScaler).motion_blur);

  // Moving in the world (world::DrawHistory) is never blurred.
  draws[0].previous_transform = glm::mat4(1.0f);
  draws[0].motion_blur = false;
  CHECK_FALSE(MakeFramePlan(view, kNoScaler).motion_blur);

  draws[1].previous_transform = glm::mat4(1.0f);
  CHECK(MakeFramePlan(view, kNoScaler).motion_blur);

  view.post.weapon_motion_blur_enabled = false;
  CHECK_FALSE(MakeFramePlan(view, kNoScaler).motion_blur);
  view.post.weapon_motion_blur_enabled = true;
  view.post.weapon_motion_blur_strength = 0.0f;
  CHECK_FALSE(MakeFramePlan(view, kNoScaler).motion_blur);
}

TEST_CASE("Overlays and the scope are planned when present") {
  SceneView view = MakeView();
  const std::array<LineVertex, 2> line{};
  view.overlay_lines = line;
  CHECK(MakeFramePlan(view, kNoScaler).overlay);
  view.overlay_lines = {};
  view.lines = line;
  CHECK(MakeFramePlan(view, kNoScaler).overlay);
  view.lines = {};
  const std::array<uint32_t, 1> selected{0};
  view.selected_draws = selected;
  CHECK(MakeFramePlan(view, kNoScaler).overlay);

  view.scope = ScopeSight{};
  view.scope->render_picture = false;
  FramePlan plan = MakeFramePlan(view, kNoScaler);
  CHECK(plan.scope);
  CHECK_FALSE(plan.scope_picture);
  view.scope->render_picture = true;
  plan = MakeFramePlan(view, kNoScaler);
  CHECK(plan.scope);
  CHECK(plan.scope_picture);
}

}  // namespace blue::render
