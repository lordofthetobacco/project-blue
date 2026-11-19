#include <doctest/doctest.h>

#include <cstddef>
#include <span>
#include <vector>

#include <glm/ext/matrix_transform.hpp>
#include <glm/mat4x4.hpp>
#include <glm/trigonometric.hpp>

#include "camera/projection.h"
#include "render/frame_globals.h"
#include "render/motion_blur_math.h"

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
                           VkExtent2D{640, 480});
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
  const VkExtent2D extent{640, 480};
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

}  // namespace blue::render
