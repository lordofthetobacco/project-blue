#include "camera/frustum.h"

#include <doctest/doctest.h>

#include <cmath>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/trigonometric.hpp>

#include "camera/projection.h"

namespace blue::camera {

namespace {

constexpr float kNear = 0.1f;

// Unit box centred at `center`.
Aabb UnitBox(glm::vec3 center) {
  return {.min = center - 0.5f, .max = center + 0.5f};
}

// A camera at the origin looking down -Z with a 60 degree vertical FOV on a
// 16:9 screen, using the renderer's reversed-Z infinite projection.
Frustum DefaultCamera() {
  const glm::mat4 view =
      glm::lookAt(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f),
                  glm::vec3(0.0f, 1.0f, 0.0f));
  return Frustum(
      ReversedInfinitePerspective(glm::radians(60.0f), 16.0f / 9.0f, kNear) *
      view);
}

}  // namespace

TEST_CASE("The infinite reversed-Z projection yields five planes") {
  CHECK(DefaultCamera().plane_count() == 5);
}

TEST_CASE("Boxes in front of the camera are visible, even very far away") {
  const Frustum frustum = DefaultCamera();
  CHECK(frustum.Intersects(UnitBox({0.0f, 0.0f, -5.0f})));
  CHECK(frustum.Intersects(UnitBox({0.0f, 0.0f, -1.0e6f})));
  CHECK(frustum.Intersects(UnitBox({2.0f, 1.0f, -5.0f})));  // inside the cone
}

TEST_CASE("Boxes behind the camera or beside the view cone are culled") {
  const Frustum frustum = DefaultCamera();
  CHECK_FALSE(frustum.Intersects(UnitBox({0.0f, 0.0f, 5.0f})));
  CHECK_FALSE(frustum.Intersects(UnitBox({0.0f, 10.0f, -5.0f})));   // above
  CHECK_FALSE(frustum.Intersects(UnitBox({-20.0f, 0.0f, -5.0f})));  // left
  // Fully between the eye and the near plane.
  CHECK_FALSE(frustum.Intersects(
      {.min = {-0.01f, -0.01f, -0.05f}, .max = {0.01f, 0.01f, -0.02f}}));
}

TEST_CASE("Boxes straddling a plane or enclosing the camera are kept") {
  const Frustum frustum = DefaultCamera();
  // Half the box is behind the camera.
  CHECK(frustum.Intersects(UnitBox({0.0f, 0.0f, 0.0f})));
  // Poking into the cone from the side.
  CHECK(frustum.Intersects(
      {.min = {2.5f, -0.5f, -5.5f}, .max = {30.0f, 0.5f, -4.5f}}));
  // A huge ground plane under the camera.
  CHECK(frustum.Intersects(
      {.min = {-500.0f, -0.1f, -500.0f}, .max = {500.0f, 0.0f, 500.0f}}));
}

TEST_CASE("The camera orientation is honoured") {
  const glm::mat4 view =
      glm::lookAt(glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f),
                  glm::vec3(0.0f, 1.0f, 0.0f));
  const Frustum frustum(
      ReversedInfinitePerspective(glm::radians(60.0f), 1.0f, kNear) * view);
  CHECK(frustum.Intersects(UnitBox({5.0f, 0.0f, 0.0f})));
  CHECK_FALSE(frustum.Intersects(UnitBox({0.0f, 0.0f, -5.0f})));
}

TEST_CASE("An orthographic box has six planes and culls on both depth ends") {
  const glm::mat4 view =
      glm::lookAt(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f),
                  glm::vec3(0.0f, 1.0f, 0.0f));
  const Frustum frustum(
      glm::orthoRH_ZO(-10.0f, 10.0f, -10.0f, 10.0f, -5.0f, 20.0f) * view);
  CHECK(frustum.plane_count() == 6);
  CHECK(frustum.Intersects(UnitBox({0.0f, 0.0f, -10.0f})));
  CHECK(frustum.Intersects(UnitBox({0.0f, 0.0f, 4.0f})));  // behind, in range
  CHECK_FALSE(frustum.Intersects(UnitBox({0.0f, 0.0f, 7.0f})));
  CHECK_FALSE(frustum.Intersects(UnitBox({0.0f, 0.0f, -25.0f})));
  CHECK_FALSE(frustum.Intersects(UnitBox({12.0f, 0.0f, -10.0f})));
}

TEST_CASE("TransformAabb translates, scales and rotates conservatively") {
  const Aabb unit = UnitBox({0.0f, 0.0f, 0.0f});

  const Aabb moved = TransformAabb(
      unit, glm::translate(glm::mat4(1.0f), glm::vec3(3.0f, -2.0f, 1.0f)));
  CHECK(moved.center().x == doctest::Approx(3.0f));
  CHECK(moved.center().y == doctest::Approx(-2.0f));
  CHECK(moved.center().z == doctest::Approx(1.0f));
  CHECK(moved.half_extents().x == doctest::Approx(0.5f));

  const Aabb scaled = TransformAabb(
      unit, glm::scale(glm::mat4(1.0f), glm::vec3(2.0f, 4.0f, 1.0f)));
  CHECK(scaled.half_extents().x == doctest::Approx(1.0f));
  CHECK(scaled.half_extents().y == doctest::Approx(2.0f));
  CHECK(scaled.half_extents().z == doctest::Approx(0.5f));

  const Aabb rotated =
      TransformAabb(unit, glm::rotate(glm::mat4(1.0f), glm::quarter_pi<float>(),
                                      glm::vec3(0.0f, 1.0f, 0.0f)));
  const float diagonal = std::sqrt(2.0f) * 0.5f;
  CHECK(rotated.half_extents().x == doctest::Approx(diagonal));
  CHECK(rotated.half_extents().z == doctest::Approx(diagonal));
  CHECK(rotated.half_extents().y == doctest::Approx(0.5f));

  // An off-centre box keeps its offset through the rotation.
  const Aabb offset =
      TransformAabb(UnitBox({1.0f, 0.0f, 0.0f}),
                    glm::rotate(glm::mat4(1.0f), glm::half_pi<float>(),
                                glm::vec3(0.0f, 1.0f, 0.0f)));
  CHECK(offset.center().x == doctest::Approx(0.0f).epsilon(1.0e-5));
  CHECK(offset.center().z == doctest::Approx(-1.0f));
}

}  // namespace blue::camera
