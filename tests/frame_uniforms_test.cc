#include <doctest/doctest.h>

#include <cstddef>
#include <span>
#include <vector>

#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>
#include <glm/mat4x4.hpp>
#include <glm/matrix.hpp>
#include <glm/trigonometric.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "camera/projection.h"
#include "core/extent.h"
#include "render/frame_uniforms.h"
#include "render/motion_blur_math.h"
#include "render/temporal_math.h"

namespace blue::render {
namespace {

// Returns a view from the origin looking down -Z with a 90 degree vertical
// field of view.
SceneView MakeView(std::span<const PointLight> lights) {
  SceneView view;
  view.camera.vertical_fov_radians = glm::radians(90.0f);
  view.camera.projection = camera::ReversedInfinitePerspective(
      view.camera.vertical_fov_radians, 1.0f, 0.1f);
  view.camera.view = glm::mat4(1.0f);
  view.point_lights = lights;
  return view;
}

FrameUniforms Uniforms(std::span<const PointLight> lights) {
  return MakeFrameUniforms(MakeView(lights), {}, /*shadows=*/false,
                           core::Extent2D{640, 480});
}

}  // namespace

TEST_CASE("An unlimited point light passes through unchanged") {
  const PointLight light{.position = {1.0f, 2.0f, 3.0f},
                         .color = {0.5f, 0.25f, 1.0f},
                         .intensity = 4.0f};
  const FrameUniforms uniforms = Uniforms({&light, 1});
  REQUIRE(uniforms.point_light_count.x == 1);
  const GpuPointLight& gpu = uniforms.point_lights[0];
  CHECK(glm::vec3(gpu.position_inverse_range_sq) == light.position);
  CHECK(gpu.position_inverse_range_sq.w == 0.0f);
  CHECK(glm::vec3(gpu.radiance) == glm::vec3(2.0f, 1.0f, 4.0f));
}

TEST_CASE("Point lights with a range are culled against the view") {
  const std::vector<PointLight> lights = {
      {.position = {0.0f, 0.0f, -5.0f}, .range = 2.0f},  // In front.
      {.position = {0.0f, 0.0f, 10.0f}, .range = 2.0f},  // Behind.
      {.position = {0.0f, 0.0f, 3.0f}, .range = 4.0f},   // Behind, reaches in.
      {.position = {0.0f, 0.0f, 50.0f}, .range = 0.0f},  // Unlimited.
      {.position = {0.0f, 0.0f, -5.0f}, .intensity = 0.0f},  // Dark.
  };
  const FrameUniforms uniforms = Uniforms(lights);
  REQUIRE(uniforms.point_light_count.x == 3);
  CHECK(uniforms.point_lights[0].position_inverse_range_sq.z == -5.0f);
  CHECK(uniforms.point_lights[0].position_inverse_range_sq.w ==
        doctest::Approx(0.25f));
  CHECK(uniforms.point_lights[1].position_inverse_range_sq.z == 3.0f);
  CHECK(uniforms.point_lights[2].position_inverse_range_sq.z == 50.0f);
}

TEST_CASE("Beyond kMaxPointLights the lights dimmest at the camera go") {
  std::vector<PointLight> lights;
  // Dim far lights first, then bright near ones: every near one must stay.
  for (size_t i = 0; i < kMaxPointLights; ++i) {
    lights.push_back({.position = {0.0f, 0.0f, -100.0f}, .intensity = 1.0f});
  }
  for (size_t i = 0; i < 4; ++i) {
    lights.push_back({.position = {0.0f, 0.0f, -2.0f}, .intensity = 2.0f});
  }
  const FrameUniforms uniforms = Uniforms(lights);
  REQUIRE(uniforms.point_light_count.x == kMaxPointLights);
  size_t near = 0;
  for (const GpuPointLight& gpu : uniforms.point_lights) {
    if (gpu.position_inverse_range_sq.z == -2.0f) {
      ++near;
    }
  }
  CHECK(near == 4);
}

TEST_CASE("No point lights gives a zero count") {
  CHECK(Uniforms({}).point_light_count.x == 0);
}

TEST_CASE("Motion blur uniforms follow the camera, settings and frame") {
  SceneView view = MakeView({});
  view.camera.previous_view_projection = glm::mat4(3.0f);
  view.post.weapon_motion_blur_strength = 2.0f;
  view.frame_seconds = 1.0f / 60.0f;
  const core::Extent2D extent{640, 480};
  const FrameUniforms uniforms = MakeFrameUniforms(view, {}, false, extent);
  CHECK(uniforms.previous_view_proj == glm::mat4(3.0f));
  CHECK(uniforms.motion_blur.x == doctest::Approx(2.0f));
  const auto radius = static_cast<float>(MotionBlurMaxRadius(480));
  CHECK(uniforms.motion_blur.y == 2.0f * radius);
  CHECK(uniforms.motion_blur.z == radius);

  view.post.weapon_motion_blur_enabled = false;
  CHECK(MakeFrameUniforms(view, {}, false, extent).motion_blur.x == 0.0f);
  view.post.weapon_motion_blur_enabled = true;
  view.frame_seconds = 0.0f;
  CHECK(MakeFrameUniforms(view, {}, false, extent).motion_blur.x == 0.0f);
}

TEST_CASE("Without jitter the uniforms are the camera's own matrices") {
  SceneView view = MakeView({});
  view.camera.view =
      glm::translate(glm::mat4(1.0f), glm::vec3(0.5f, -1.0f, 2.0f));
  const FrameUniforms uniforms =
      MakeFrameUniforms(view, {}, false, core::Extent2D{640, 480});
  CHECK(uniforms.projection == view.camera.projection);
  CHECK(uniforms.view_proj == view.camera.projection * view.camera.view);
  CHECK(uniforms.unjittered_view_proj == uniforms.view_proj);
  CHECK(uniforms.jitter == glm::vec4(0.0f));
}

TEST_CASE("Jitter moves what is rasterized and nothing else") {
  SceneView view = MakeView({});
  view.camera.view =
      glm::translate(glm::mat4(1.0f), glm::vec3(0.5f, -1.0f, 2.0f));
  view.camera.previous_view_projection = glm::mat4(3.0f);
  const core::Extent2D extent{640, 480};
  const glm::vec2 jitter(0.25f, -0.375f);
  const FrameUniforms plain = MakeFrameUniforms(view, {}, false, extent);
  const FrameUniforms uniforms =
      MakeFrameUniforms(view, {}, false, extent, jitter);

  CHECK(uniforms.projection ==
        JitterProjection(view.camera.projection, jitter, extent));
  CHECK(uniforms.view_proj == uniforms.projection * view.camera.view);
  CHECK(uniforms.inverse_projection == glm::inverse(uniforms.projection));
  CHECK(uniforms.inverse_view_proj == glm::inverse(uniforms.view_proj));
  CHECK(uniforms.jitter == glm::vec4(jitter, 0.0f, 0.0f));
  // Motion and culling go by the camera as it is.
  CHECK(uniforms.unjittered_view_proj == plain.view_proj);
  CHECK(uniforms.previous_view_proj == glm::mat4(3.0f));
  CHECK(uniforms.view == plain.view);
  CHECK(uniforms.screen == plain.screen);

  // A point lands `jitter` pixels further right and down.
  const glm::vec4 world(0.3f, 0.2f, -5.0f, 1.0f);
  const glm::vec2 size(640.0f, 480.0f);
  const auto pixel = [&size](const glm::vec4& clip) {
    const glm::vec2 ndc = glm::vec2(clip) / clip.w;
    return glm::vec2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f) * size;
  };
  const glm::vec4 clip = uniforms.view_proj * world;
  const glm::vec2 moved = pixel(clip) - pixel(plain.view_proj * world);
  CHECK(moved.x == doctest::Approx(jitter.x).epsilon(1.0e-3));
  CHECK(moved.y == doctest::Approx(jitter.y).epsilon(1.0e-3));

  // shaders/motion_velocity.vert.metal takes the jitter out of a clip
  // position again with this expression.
  glm::vec4 unjittered = clip;
  unjittered.x -= 2.0f * uniforms.jitter.x * uniforms.screen.z * clip.w;
  unjittered.y -= -2.0f * uniforms.jitter.y * uniforms.screen.w * clip.w;
  const glm::vec4 expected = plain.view_proj * world;
  CHECK(unjittered.x == doctest::Approx(expected.x).epsilon(1.0e-5));
  CHECK(unjittered.y == doctest::Approx(expected.y).epsilon(1.0e-5));
  CHECK(unjittered.z == expected.z);
  CHECK(unjittered.w == expected.w);
}

// shaders/motion_camera.frag.metal, expression for expression: the motion
// vector of the surface point seen at `pixel` (its centre) with `depth`.
glm::vec2 CameraMotionAt(const FrameUniforms& frame, glm::vec2 pixel,
                         float depth) {
  const glm::vec2 uv = pixel * glm::vec2(frame.screen.z, frame.screen.w);
  const glm::vec4 world =
      frame.inverse_view_proj *
      glm::vec4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, depth, 1.0f);
  return MotionVector(frame.unjittered_view_proj * world,
                      frame.previous_view_proj * world);
}

TEST_CASE("Camera motion vectors are found again from the depth buffer") {
  SceneView view = MakeView({});
  // The camera moved and turned since the previous frame.
  const glm::mat4 previous_view =
      glm::translate(glm::mat4(1.0f), glm::vec3(0.1f, 0.0f, 0.3f));
  view.camera.view = glm::rotate(glm::mat4(1.0f), glm::radians(2.0f),
                                 glm::vec3(0.0f, 1.0f, 0.0f)) *
                     glm::translate(glm::mat4(1.0f),
                                    glm::vec3(-0.2f, 0.05f, 0.1f));
  view.camera.previous_view_projection =
      view.camera.projection * previous_view;
  const core::Extent2D extent{640, 480};
  const glm::vec2 size(640.0f, 480.0f);

  for (const glm::vec2 jitter :
       {glm::vec2(0.0f), glm::vec2(0.3f, -0.45f), glm::vec2(-0.5f, 0.5f)}) {
    CAPTURE(jitter.x);
    const FrameUniforms frame =
        MakeFrameUniforms(view, {}, false, extent, jitter);
    for (const glm::vec4& world :
         {glm::vec4(0.4f, -0.3f, -3.0f, 1.0f),
          glm::vec4(-20.0f, 5.0f, -400.0f, 1.0f),
          // The sky: a direction, at depth 0 of the reversed-Z projection.
          glm::vec4(0.2f, 0.3f, -1.0f, 0.0f)}) {
      CAPTURE(world.z);
      // What the rasterizer stores for the point: its pixel and depth
      // under the jittered view-projection.
      const glm::vec4 clip = frame.view_proj * world;
      const glm::vec2 ndc = glm::vec2(clip) / clip.w;
      const glm::vec2 pixel =
          glm::vec2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f) * size;
      const float depth = clip.z / clip.w;
      if (world.w == 0.0f) {
        CHECK(depth == 0.0f);
      }
      const glm::vec2 expected =
          MotionVector(frame.unjittered_view_proj * world,
                       frame.previous_view_proj * world);
      const glm::vec2 found = CameraMotionAt(frame, pixel, depth);
      // Within a hundredth of a pixel.
      CHECK(found.x * size.x ==
            doctest::Approx(expected.x * size.x).epsilon(1.0e-2));
      CHECK(found.y * size.y ==
            doctest::Approx(expected.y * size.y).epsilon(1.0e-2));
      CHECK(glm::length(expected * size) > 1.0f);  // It did move.
    }
  }
}

TEST_CASE("A camera that stands still has no camera motion at all") {
  SceneView view = MakeView({});
  view.camera.view =
      glm::translate(glm::mat4(1.0f), glm::vec3(0.5f, -1.0f, 2.0f));
  view.camera.previous_view_projection =
      view.camera.projection * view.camera.view;
  const FrameUniforms frame = MakeFrameUniforms(
      view, {}, false, core::Extent2D{640, 480}, glm::vec2(0.3f, -0.2f));
  // Exactly zero, whatever the depth: both projections are the same matrix.
  CHECK(CameraMotionAt(frame, {100.5f, 200.5f}, 0.02f) == glm::vec2(0.0f));
  CHECK(CameraMotionAt(frame, {639.5f, 0.5f}, 0.0f) == glm::vec2(0.0f));
}

}  // namespace blue::render
