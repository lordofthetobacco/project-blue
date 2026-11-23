#include "render/motion_blur_math.h"

#include <doctest/doctest.h>

#include <cmath>
#include <limits>
#include <optional>

#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>
#include <glm/mat4x4.hpp>
#include <glm/matrix.hpp>
#include <glm/trigonometric.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "camera/projection.h"

// ScreenVelocity() and BlurVector() mirror shaders/motion_blur.h.

namespace blue::render {
namespace {

constexpr float kNan = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();
const glm::vec2 kExtent(1000.0f, 1000.0f);

// A 90 degree, square projection: a point at depth d and height h is at
// NDC h / d.
glm::mat4 Projection() {
  return camera::ReversedInfinitePerspective(glm::radians(90.0f), 1.0f, 0.1f);
}

// Returns the screen velocity of local point `point` of an item moving from
// `previous_model` under `previous_view` to `model` under `view`.
glm::vec2 VelocityOf(const glm::vec3& point, const glm::mat4& previous_model,
                     const glm::mat4& previous_view, const glm::mat4& model,
                     const glm::mat4& view) {
  const glm::mat4 projection = Projection();
  const glm::vec4 local(point, 1.0f);
  return ScreenVelocity(projection * view * (model * local),
                        projection * previous_view * (previous_model * local),
                        kExtent);
}

bool IsFinite(const glm::vec2& v) {
  return std::isfinite(v.x) && std::isfinite(v.y);
}

TEST_CASE("An item rigidly attached to the camera has no velocity") {
  // A weapon-like offset in camera space, under large camera moves.
  const glm::mat4 in_camera =
      glm::translate(glm::mat4(1.0f), glm::vec3(0.1f, -0.15f, -0.3f));
  const glm::mat4 previous_view = glm::inverse(
      glm::translate(glm::mat4(1.0f), glm::vec3(10.0f, 1.7f, -4.0f)) *
      glm::rotate(glm::mat4(1.0f), 0.3f, glm::vec3(0.0f, 1.0f, 0.0f)));
  const glm::mat4 view = glm::inverse(
      glm::translate(glm::mat4(1.0f), glm::vec3(10.5f, 1.6f, -4.3f)) *
      glm::rotate(glm::mat4(1.0f), 1.2f, glm::vec3(0.0f, 1.0f, 0.0f)) *
      glm::rotate(glm::mat4(1.0f), -0.4f, glm::vec3(1.0f, 0.0f, 0.0f)));
  for (const glm::vec3& point :
       {glm::vec3(0.0f), glm::vec3(0.05f, 0.02f, -0.4f),
        glm::vec3(-0.03f, -0.1f, 0.1f)}) {
    const glm::vec2 velocity =
        VelocityOf(point, glm::inverse(previous_view) * in_camera,
                   previous_view, glm::inverse(view) * in_camera, view);
    CHECK(glm::length(velocity) < 1.0e-2f);
    CHECK(BlurVector(velocity, 1.0f, 54.0f) == glm::vec2(0.0f));
  }
}

TEST_CASE("A known translation gives the expected pixel motion") {
  const glm::mat4 still(1.0f);
  // At depth 2 a move of 0.2 m is 0.1 in NDC, 50 pixels on 1000.
  const glm::mat4 right =
      glm::translate(glm::mat4(1.0f), glm::vec3(0.2f, 0.0f, 0.0f));
  const glm::vec2 to_right =
      VelocityOf({0.0f, 0.0f, -2.0f}, still, still, right, still);
  CHECK(to_right.x == doctest::Approx(50.0f).epsilon(1.0e-4));
  CHECK(to_right.y == doctest::Approx(0.0f));
  // Up in the world is up on screen: negative pixel rows.
  const glm::mat4 up =
      glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.2f, 0.0f));
  const glm::vec2 to_top =
      VelocityOf({0.0f, 0.0f, -2.0f}, still, still, up, still);
  CHECK(to_top.x == doctest::Approx(0.0f));
  CHECK(to_top.y == doctest::Approx(-50.0f).epsilon(1.0e-4));
}

TEST_CASE("A known camera rotation gives the expected pixel motion") {
  const float angle = 0.1f;
  const glm::mat4 still(1.0f);
  // A view that turns the world by `angle` about +y (the camera turning
  // right) moves a point straight ahead to the left by tan(angle) in NDC.
  const glm::mat4 view =
      glm::rotate(glm::mat4(1.0f), angle, glm::vec3(0.0f, 1.0f, 0.0f));
  const glm::vec2 velocity =
      VelocityOf({0.0f, 0.0f, -3.0f}, still, still, still, view);
  CHECK(velocity.x ==
        doctest::Approx(-std::tan(angle) * 500.0f).epsilon(1.0e-4));
  CHECK(velocity.y == doctest::Approx(0.0f));
}

TEST_CASE("Identical matrices give exactly zero velocity") {
  const glm::mat4 model =
      glm::translate(glm::mat4(1.0f), glm::vec3(0.3f, -0.2f, -1.0f));
  const glm::mat4 view =
      glm::rotate(glm::mat4(1.0f), 0.7f, glm::vec3(0.0f, 1.0f, 0.0f));
  CHECK(VelocityOf({0.1f, 0.2f, 0.3f}, model, view, model, view) ==
        glm::vec2(0.0f));
}

TEST_CASE("Points behind a camera and broken inputs give finite zeros") {
  const glm::vec4 ahead(0.1f, 0.2f, 0.05f, 1.0f);
  CHECK(ScreenVelocity(ahead, {0.1f, 0.2f, 0.05f, -1.0f}, kExtent) ==
        glm::vec2(0.0f));
  CHECK(ScreenVelocity(ahead, {0.1f, 0.2f, 0.05f, 0.0f}, kExtent) ==
        glm::vec2(0.0f));
  CHECK(ScreenVelocity({0.1f, 0.2f, 0.05f, -2.0f}, ahead, kExtent) ==
        glm::vec2(0.0f));
  CHECK(ScreenVelocity(ahead, {kNan, 0.0f, 0.0f, 1.0f}, kExtent) ==
        glm::vec2(0.0f));
  CHECK(ScreenVelocity(ahead, {kInf, 0.0f, 0.0f, 1.0f}, kExtent) ==
        glm::vec2(0.0f));
  CHECK(ScreenVelocity(ahead, ahead, kExtent) == glm::vec2(0.0f));
  // A tiny positive w (a point grazing the camera) stays finite.
  CHECK(IsFinite(ScreenVelocity(ahead, {1.0f, 1.0f, 0.0f, 2.0e-6f}, kExtent)));
  CHECK(IsFinite(BlurVector({kInf, 0.0f}, 1.0f, 54.0f)));
  CHECK(BlurVector({kNan, 1.0f}, 1.0f, 54.0f) == glm::vec2(0.0f));
}

TEST_CASE("The blur scale is a fixed exposure, independent of frame rate") {
  CHECK(MotionBlurScale(1.0f, 1.0f / 60.0f) == doctest::Approx(1.0f));
  CHECK(MotionBlurScale(2.0f, 1.0f / 60.0f) == doctest::Approx(2.0f));
  // At 120 Hz a frame moves half as far: twice the scale, same blur.
  CHECK(MotionBlurScale(1.0f, 1.0f / 120.0f) == doctest::Approx(2.0f));
  CHECK(MotionBlurScale(1.0f, 1.0f / 30.0f) == doctest::Approx(0.5f));
  // No blur: off, paused, broken or hitching frames.
  CHECK(MotionBlurScale(0.0f, 1.0f / 60.0f) == 0.0f);
  CHECK(MotionBlurScale(-1.0f, 1.0f / 60.0f) == 0.0f);
  CHECK(MotionBlurScale(kNan, 1.0f / 60.0f) == 0.0f);
  CHECK(MotionBlurScale(1.0f, 0.0f) == 0.0f);
  CHECK(MotionBlurScale(1.0f, -0.01f) == 0.0f);
  CHECK(MotionBlurScale(1.0f, kNan) == 0.0f);
  CHECK(MotionBlurScale(1.0f, kInf) == 0.0f);
  CHECK(MotionBlurScale(1.0f, kMotionBlurMaxFrameSeconds * 1.01f) == 0.0f);
  CHECK(MotionBlurScale(1.0f, kMotionBlurMaxFrameSeconds) > 0.0f);
}

TEST_CASE("Blur vectors: scale, dead zone, ramp and clamp") {
  const float max_length = 54.0f;
  // Scaled by strength and frame duration.
  CHECK(BlurVector({10.0f, 0.0f}, 2.0f, max_length) == glm::vec2(20.0f, 0.0f));
  // Below half a pixel: nothing, so the idle bob stays sharp.
  CHECK(BlurVector({0.4f, 0.0f}, 1.0f, max_length) == glm::vec2(0.0f));
  CHECK(BlurVector({0.3f, 0.3f}, 1.0f, max_length) == glm::vec2(0.0f));
  CHECK(BlurVector({0.2f, 0.0f}, 2.0f, max_length) == glm::vec2(0.0f));
  // From 0.5 to 1 pixel it ramps up continuously, then is unchanged.
  CHECK(glm::length(BlurVector({0.5f, 0.0f}, 1.0f, max_length)) ==
        doctest::Approx(0.0f));
  CHECK(glm::length(BlurVector({0.75f, 0.0f}, 1.0f, max_length)) ==
        doctest::Approx(0.5f));
  CHECK(glm::length(BlurVector({0.0f, -1.0f}, 1.0f, max_length)) ==
        doctest::Approx(1.0f));
  CHECK(BlurVector({0.0f, -3.0f}, 1.0f, max_length) == glm::vec2(0.0f, -3.0f));
  // Clamped to the longest blur, keeping the direction.
  const glm::vec2 clamped = BlurVector({300.0f, 400.0f}, 1.0f, max_length);
  CHECK(glm::length(clamped) == doctest::Approx(max_length));
  CHECK(clamped.x / clamped.y == doctest::Approx(0.75f));
  // A zero scale (blur off) gives nothing.
  CHECK(BlurVector({100.0f, 0.0f}, 0.0f, max_length) == glm::vec2(0.0f));
}

TEST_CASE("The largest blur radius follows the target height") {
  CHECK(MotionBlurMaxRadius(1080) == 27);
  CHECK(MotionBlurMaxRadius(2160) == 54);
  CHECK(MotionBlurMaxRadius(720) == 18);
  CHECK(MotionBlurMaxRadius(10) == 1);
  CHECK(MotionBlurMaxRadius(0) == 1);
}
TEST_CASE("A box in front of the camera projects to its exact bounds") {
  const std::optional<ScreenBounds> bounds = ProjectBoxToScreen(
      {-0.1f, -0.1f, -2.1f}, {0.1f, 0.1f, -1.9f}, Projection(), kExtent);
  REQUIRE(bounds.has_value());
  // The near face (depth 1.9) is the widest: 0.1 / 1.9 in NDC.
  const float half = 0.1f / 1.9f * 500.0f;
  CHECK(bounds->min.x == doctest::Approx(500.0f - half));
  CHECK(bounds->max.x == doctest::Approx(500.0f + half));
  CHECK(bounds->min.y == doctest::Approx(500.0f - half));
  CHECK(bounds->max.y == doctest::Approx(500.0f + half));
}

TEST_CASE("A box straddling the camera is bounded by its visible part") {
  // Like the held rifle: the stock reaches behind the eye.
  const glm::vec3 box_min(0.2f, -0.1f, -1.0f);
  const glm::vec3 box_max(0.4f, 0.1f, 0.5f);
  const glm::mat4 projection = Projection();
  const std::optional<ScreenBounds> bounds =
      ProjectBoxToScreen(box_min, box_max, projection, kExtent);
  REQUIRE(bounds.has_value());
  REQUIRE(IsFinite(bounds->min));
  REQUIRE(IsFinite(bounds->max));
  // Leftmost: x = 0.2 at the far end (1 m), NDC 0.2. Rightmost: x = 0.4 at
  // the near plane (0.1 m), NDC 4 (off screen; the caller clamps).
  CHECK(bounds->min.x == doctest::Approx(500.0f + 0.2f / 1.0f * 500.0f));
  CHECK(bounds->max.x == doctest::Approx(500.0f + 0.4f / 0.1f * 500.0f));

  // Every sampled point of the box in front of the near plane is inside.
  constexpr int kSteps = 16;
  constexpr float kSlack = 1.0e-2f;
  const float near_plane = 0.1f;
  int checked = 0;
  for (int i = 0; i <= kSteps; ++i) {
    for (int j = 0; j <= kSteps; ++j) {
      for (int k = 0; k <= kSteps; ++k) {
        const glm::vec3 t(static_cast<float>(i), static_cast<float>(j),
                          static_cast<float>(k));
        const glm::vec3 point =
            box_min + (box_max - box_min) * (t / static_cast<float>(kSteps));
        if (-point.z < near_plane) {
          continue;
        }
        const glm::vec4 clip = projection * glm::vec4(point, 1.0f);
        const glm::vec2 ndc = glm::vec2(clip) / clip.w;
        const glm::vec2 pixel((ndc.x * 0.5f + 0.5f) * kExtent.x,
                              (0.5f - ndc.y * 0.5f) * kExtent.y);
        CHECK(pixel.x >= bounds->min.x - kSlack);
        CHECK(pixel.y >= bounds->min.y - kSlack);
        CHECK(pixel.x <= bounds->max.x + kSlack);
        CHECK(pixel.y <= bounds->max.y + kSlack);
        ++checked;
      }
    }
  }
  CHECK(checked > 0);
}

TEST_CASE("A box behind the camera or before the near plane is empty") {
  CHECK_FALSE(ProjectBoxToScreen({-0.1f, -0.1f, 0.5f}, {0.1f, 0.1f, 1.0f},
                                 Projection(), kExtent)
                  .has_value());
  CHECK_FALSE(ProjectBoxToScreen({-0.1f, -0.1f, -0.05f}, {0.1f, 0.1f, -0.01f},
                                 Projection(), kExtent)
                  .has_value());
}

}  // namespace
}  // namespace blue::render
