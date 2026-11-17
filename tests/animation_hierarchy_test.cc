#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <numbers>

#include <glm/ext/quaternion_trigonometric.hpp>
#include <glm/vec4.hpp>

#include "animation/hierarchy.h"
#include "animation/transform.h"

namespace blue::animation {
namespace {

constexpr float kEpsilon = 1.0e-5f;

bool NearlyEqual(const glm::vec3& a, const glm::vec3& b) {
  return glm::all(glm::lessThanEqual(glm::abs(a - b), glm::vec3(kEpsilon)));
}

glm::vec3 TransformPoint(const glm::mat4& matrix, const glm::vec3& point) {
  return glm::vec3(matrix * glm::vec4(point, 1.0f));
}

// Root translated by (1, 0, 0), child rotated 90 degrees about +Y and scaled
// by 2, grandchild translated by (0, 0, 1) from the child.
Hierarchy MakeChain() {
  Hierarchy hierarchy;
  hierarchy.nodes.push_back({.name = "root",
                             .parent = -1,
                             .rest = {.translation = {1.0f, 0.0f, 0.0f}}});
  hierarchy.nodes.push_back(
      {.name = "child",
       .parent = 0,
       .rest = {.rotation = glm::angleAxis(std::numbers::pi_v<float> / 2.0f,
                                           glm::vec3(0.0f, 1.0f, 0.0f)),
                .scale = glm::vec3(2.0f)}});
  hierarchy.nodes.push_back({.name = "leaf",
                             .parent = 1,
                             .rest = {.translation = {0.0f, 0.0f, 1.0f}}});
  return hierarchy;
}

}  // namespace

TEST_CASE("ToMatrix applies scale, then rotation, then translation") {
  const Transform transform{
      .translation = {0.0f, 5.0f, 0.0f},
      .rotation = glm::angleAxis(std::numbers::pi_v<float> / 2.0f,
                                 glm::vec3(0.0f, 0.0f, 1.0f)),
      .scale = {3.0f, 1.0f, 1.0f}};
  // (1,0,0) -> scale (3,0,0) -> rotate about Z (0,3,0) -> translate (0,8,0).
  CHECK(NearlyEqual(TransformPoint(ToMatrix(transform), {1.0f, 0.0f, 0.0f}),
                    {0.0f, 8.0f, 0.0f}));
  CHECK(NearlyEqual(TransformPoint(ToMatrix(Transform{}), {1.0f, 2.0f, 3.0f}),
                    {1.0f, 2.0f, 3.0f}));
}

TEST_CASE("IsValid requires parents before children") {
  Hierarchy hierarchy = MakeChain();
  CHECK(IsValid(hierarchy));
  CHECK(IsValid(Hierarchy{}));
  hierarchy.nodes[0].parent = 2;
  CHECK_FALSE(IsValid(hierarchy));
  hierarchy.nodes[0].parent = 0;  // its own parent
  CHECK_FALSE(IsValid(hierarchy));
  hierarchy.nodes[0].parent = -2;
  CHECK_FALSE(IsValid(hierarchy));
}

TEST_CASE("FindNode returns the first match or -1") {
  Hierarchy hierarchy = MakeChain();
  CHECK(FindNode(hierarchy, "root") == 0);
  CHECK(FindNode(hierarchy, "leaf") == 2);
  CHECK(FindNode(hierarchy, "missing") == -1);
  hierarchy.nodes.push_back({.name = "root", .parent = 0, .rest = {}});
  CHECK(FindNode(hierarchy, "root") == 0);
}

TEST_CASE("ComputeWorldMatrices composes parent transforms") {
  const Hierarchy hierarchy = MakeChain();
  std::array<Transform, 3> pose;
  ResetToRest(hierarchy, pose);
  std::array<glm::mat4, 3> world;
  ComputeWorldMatrices(hierarchy, pose, world);

  CHECK(NearlyEqual(TransformPoint(world[0], {}), {1.0f, 0.0f, 0.0f}));
  // Leaf origin: (0,0,1) scaled by 2 -> (0,0,2), rotated 90 deg about Y ->
  // (2,0,0), plus the root's (1,0,0).
  CHECK(NearlyEqual(TransformPoint(world[2], {}), {3.0f, 0.0f, 0.0f}));
  // A unit +Z step in leaf space is scaled and rotated like the child.
  CHECK(NearlyEqual(TransformPoint(world[2], {0.0f, 0.0f, 1.0f}),
                    {5.0f, 0.0f, 0.0f}));

  // Moving the root moves the whole chain.
  pose[0].translation = {0.0f, 1.0f, 0.0f};
  ComputeWorldMatrices(hierarchy, pose, world);
  CHECK(NearlyEqual(TransformPoint(world[2], {}), {2.0f, 1.0f, 0.0f}));
}

TEST_CASE("ComputeWorldMatrices processes only what fits") {
  const Hierarchy hierarchy = MakeChain();
  std::array<Transform, 3> pose;
  ResetToRest(hierarchy, pose);
  std::array<glm::mat4, 2> world{glm::mat4(0.0f), glm::mat4(0.0f)};
  ComputeWorldMatrices(hierarchy, pose, world);
  CHECK(NearlyEqual(TransformPoint(world[1], {}), {1.0f, 0.0f, 0.0f}));
}

}  // namespace blue::animation
