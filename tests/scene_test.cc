#include "scene/scene.h"

#include <doctest/doctest.h>

#include <limits>

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace blue::scene {

TEST_CASE("transform matrix is translate * rotate * scale") {
  const Transform transform{
      {1.0f, 2.0f, 3.0f},
      glm::angleAxis(0.7f, glm::normalize(glm::vec3(1.0f, 1.0f, 0.0f))),
      {2.0f, 3.0f, 4.0f}};
  const glm::mat4 expected =
      glm::translate(glm::mat4(1.0f), transform.position) *
      glm::mat4_cast(transform.rotation) *
      glm::scale(glm::mat4(1.0f), transform.scale);
  CHECK(transform.Matrix() == expected);
}

TEST_CASE("ValidateRigidBody accepts the defaults and each valid shape") {
  CHECK_FALSE(ValidateRigidBody(RigidBody{}).has_value());

  RigidBody body;
  body.shape = ShapeType::kSphere;
  CHECK_FALSE(ValidateRigidBody(body).has_value());
  body.shape = ShapeType::kConvexHull;
  CHECK_FALSE(ValidateRigidBody(body).has_value());

  body.motion = MotionType::kStatic;
  body.shape = ShapeType::kTriangleMesh;
  body.mass = 0.0f;  // Mass is ignored for static bodies.
  CHECK_FALSE(ValidateRigidBody(body).has_value());

  body = RigidBody{};
  body.friction = 0.0f;
  body.restitution = 1.0f;
  CHECK_FALSE(ValidateRigidBody(body).has_value());
}

TEST_CASE("ValidateRigidBody names the reason for an invalid body") {
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();
  RigidBody body;

  SUBCASE("non-finite values") {
    body.mass = nan;
    CHECK(ValidateRigidBody(body).has_value());
    body = RigidBody{};
    body.friction = inf;
    CHECK(ValidateRigidBody(body).has_value());
    body = RigidBody{};
    body.restitution = nan;
    CHECK(ValidateRigidBody(body).has_value());
    // A static body's mass is unused but must still be finite.
    body = RigidBody{};
    body.motion = MotionType::kStatic;
    body.mass = nan;
    CHECK(ValidateRigidBody(body).has_value());
  }

  SUBCASE("dynamic mass must be positive") {
    body.mass = 0.0f;
    CHECK(ValidateRigidBody(body).has_value());
    body.mass = -1.0f;
    CHECK(ValidateRigidBody(body).has_value());
  }

  SUBCASE("friction must not be negative") {
    body.friction = -0.01f;
    CHECK(ValidateRigidBody(body).has_value());
  }

  SUBCASE("restitution must lie in [0, 1]") {
    body.restitution = -0.01f;
    CHECK(ValidateRigidBody(body).has_value());
    body.restitution = 1.01f;
    CHECK(ValidateRigidBody(body).has_value());
  }

  SUBCASE("triangle mesh needs a static body") {
    body.shape = ShapeType::kTriangleMesh;
    CHECK(ValidateRigidBody(body).has_value());
  }
}

}  // namespace blue::scene

namespace blue::scene {

TEST_CASE("the sun defaults to a normalized, shadow-casting light") {
  const SunLight sun;
  CHECK(glm::length(sun.direction) == doctest::Approx(1.0f).epsilon(1e-5));
  const glm::vec3 expected = glm::normalize(glm::vec3(-0.4f, -1.0f, -0.3f));
  CHECK(sun.direction.x == doctest::Approx(expected.x).epsilon(1e-5));
  CHECK(sun.direction.y == doctest::Approx(expected.y).epsilon(1e-5));
  CHECK(sun.direction.z == doctest::Approx(expected.z).epsilon(1e-5));
  CHECK(sun.intensity == 3.0f);
  CHECK(sun.casts_shadows);
  CHECK_FALSE(ValidateSunLight(sun).has_value());
}

TEST_CASE("ValidateSunLight accepts edge values and names invalid ones") {
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();
  SunLight sun;

  SUBCASE("valid edge values") {
    sun.direction = {5.0f, 0.0f, 0.0f};  // Need not be normalized.
    sun.color = glm::vec3(0.0f);
    sun.intensity = 0.0f;
    CHECK_FALSE(ValidateSunLight(sun).has_value());
  }

  SUBCASE("direction") {
    sun.direction = glm::vec3(0.0f);
    CHECK(ValidateSunLight(sun).has_value());
    sun.direction = {1e-7f, 0.0f, 0.0f};
    CHECK(ValidateSunLight(sun).has_value());
    sun.direction = {nan, -1.0f, 0.0f};
    CHECK(ValidateSunLight(sun).has_value());
    sun.direction = {0.0f, inf, 0.0f};
    CHECK(ValidateSunLight(sun).has_value());
  }

  SUBCASE("color") {
    sun.color.x = -0.01f;
    CHECK(ValidateSunLight(sun).has_value());
    sun.color = {1.0f, 1.0f, nan};
    CHECK(ValidateSunLight(sun).has_value());
  }

  SUBCASE("intensity") {
    sun.intensity = -0.01f;
    CHECK(ValidateSunLight(sun).has_value());
    sun.intensity = inf;
    CHECK(ValidateSunLight(sun).has_value());
  }
}

}  // namespace blue::scene
