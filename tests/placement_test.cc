#include "scene/placement.h"

#include <doctest/doctest.h>

#include <cmath>

namespace blue::scene {

namespace {

constexpr float kEpsilon = 1.0e-5f;

bool NearlyEqual(float a, float b) { return std::abs(a - b) <= kEpsilon; }

}  // namespace

TEST_CASE("FitOnGround scales the largest extent to the target size") {
  const Transform t = FitOnGround({-1.0f, 0.0f, -0.5f}, {3.0f, 1.0f, 0.5f},
                                  2.0f, {0.0f, 0.0f, 0.0f});

  CHECK(NearlyEqual(t.scale.x, 0.5f));  // Largest extent is 4 (x).
  CHECK(t.scale.x == t.scale.y);
  CHECK(t.scale.y == t.scale.z);
}

TEST_CASE(
    "FitOnGround centres an off-centre box on the ground point and rests it on "
    "the ground") {
  const glm::vec3 bounds_min{-3.0f, 1.0f, 2.0f};
  const glm::vec3 bounds_max{1.0f, 3.0f, 4.0f};
  const glm::vec3 ground{5.0f, 0.25f, -2.0f};

  const Transform t = FitOnGround(bounds_min, bounds_max, 2.0f, ground);
  const glm::mat4 matrix = t.Matrix();

  const glm::vec3 world_min = glm::vec3(matrix * glm::vec4(bounds_min, 1.0f));
  const glm::vec3 world_max = glm::vec3(matrix * glm::vec4(bounds_max, 1.0f));

  CHECK(NearlyEqual((world_min.x + world_max.x) * 0.5f, ground.x));
  CHECK(NearlyEqual((world_min.z + world_max.z) * 0.5f, ground.z));
  CHECK(NearlyEqual(world_min.y, ground.y));
  CHECK(NearlyEqual(world_max.x - world_min.x, 2.0f));
}

TEST_CASE("FitOnGround keeps scale 1 for a box without extent") {
  const glm::vec3 point{1.0f, 2.0f, 3.0f};
  const Transform t = FitOnGround(point, point, 2.0f, {0.0f, 0.0f, 0.0f});

  CHECK(t.scale == glm::vec3(1.0f));
  CHECK(std::isfinite(t.position.x));
  CHECK(std::isfinite(t.position.y));
  CHECK(std::isfinite(t.position.z));
  CHECK(NearlyEqual(t.position.y,
                    -2.0f));  // The point itself rests on the ground.
}

}  // namespace blue::scene
