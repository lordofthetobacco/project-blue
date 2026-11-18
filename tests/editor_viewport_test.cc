#include <doctest/doctest.h>

#include <cmath>

#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/vec4.hpp>

#include "animation/transform.h"
#include "editor/viewport/debug_draw.h"
#include "editor/viewport/editor_camera.h"
#include "editor/viewport/gizmo.h"
#include "render/debug_lines.h"

namespace blue::editor {
namespace {

bool Near(const glm::vec3& a, const glm::vec3& b, float epsilon = 1.0e-4f) {
  return glm::length(a - b) <= epsilon;
}

}  // namespace

TEST_CASE("EditorCamera keeps the eye at distance from the target") {
  EditorCamera camera;
  camera.LookAt({0.0f, 2.0f, 10.0f}, {0.0f, 2.0f, 0.0f});
  CHECK(Near(camera.position(), {0.0f, 2.0f, 10.0f}));
  CHECK(Near(camera.forward(), {0.0f, 0.0f, -1.0f}));
  CHECK(camera.distance() == doctest::Approx(10.0f));

  camera.Orbit({0.3f, 0.2f});
  CHECK(glm::distance(camera.position(), camera.target()) ==
        doctest::Approx(10.0f));
  CHECK(Near(camera.target(), {0.0f, 2.0f, 0.0f}));
}

TEST_CASE("EditorCamera pitch is clamped short of the poles") {
  EditorCamera camera;
  camera.Orbit({0.0f, 10.0f});
  CHECK(camera.pitch() < 1.5708f);
  camera.Orbit({0.0f, -20.0f});
  CHECK(camera.pitch() > -1.5708f);
  // The view matrix stays finite.
  CHECK(std::isfinite(camera.view()[0][0]));
}

TEST_CASE("EditorCamera Look turns in place, Move translates eye and target") {
  EditorCamera camera;
  camera.LookAt({1.0f, 1.0f, 5.0f}, {1.0f, 1.0f, 0.0f});
  const glm::vec3 eye = camera.position();
  camera.Look({0.5f, -0.1f});
  CHECK(Near(camera.position(), eye, 1.0e-3f));

  camera.LookAt({0.0f, 0.0f, 5.0f}, {0.0f, 0.0f, 0.0f});
  camera.Move({1.0f, 0.0f, 2.0f});  // right 1, forward 2
  CHECK(Near(camera.position(), {1.0f, 0.0f, 3.0f}));
  CHECK(Near(camera.target(), {1.0f, 0.0f, -2.0f}));
}

TEST_CASE("EditorCamera Dolly and Frame keep the distance in range") {
  EditorCamera camera;
  camera.Frame({3.0f, 0.0f, 0.0f}, 1.0f);
  CHECK(Near(camera.target(), {3.0f, 0.0f, 0.0f}));
  const float framed = camera.distance();
  CHECK(framed > 1.0f);
  camera.Dolly(1.0f);
  CHECK(camera.distance() < framed);
  camera.Dolly(1000.0f);
  CHECK(camera.distance() == doctest::Approx(EditorCamera::kMinDistance));
  camera.Dolly(-10000.0f);
  CHECK(camera.distance() == doctest::Approx(EditorCamera::kMaxDistance));
}

TEST_CASE("EditorCamera ScreenRay goes through the projected point") {
  EditorCamera camera;
  camera.LookAt({0.0f, 0.0f, 5.0f}, {0.0f, 0.0f, 0.0f});
  const Ray center = camera.ScreenRay({0.0f, 0.0f}, 1.5f);
  CHECK(Near(center.origin, camera.position()));
  CHECK(Near(center.direction, camera.forward()));

  // A world point projected to NDC and back lands on the ray.
  const glm::vec3 point(1.0f, 0.5f, -2.0f);
  const glm::vec4 clip =
      camera.projection(1.5f) * camera.view() * glm::vec4(point, 1.0f);
  const Ray ray = camera.ScreenRay({clip.x / clip.w, clip.y / clip.w}, 1.5f);
  const glm::vec3 to_point = glm::normalize(point - ray.origin);
  CHECK(glm::dot(to_point, ray.direction) == doctest::Approx(1.0f));
}

TEST_CASE("LocalFromWorld inverts parent * ToMatrix(local)") {
  animation::Transform local;
  local.translation = {1.0f, -2.0f, 3.0f};
  local.rotation = glm::angleAxis(0.7f, glm::normalize(glm::vec3(1, 2, 3)));
  local.scale = {2.0f, 0.5f, 1.5f};
  const glm::mat4 parent =
      glm::rotate(glm::translate(glm::mat4(1.0f), {5.0f, 0.0f, 0.0f}), 1.1f,
                  glm::vec3(0.0f, 1.0f, 0.0f));
  const glm::mat4 world = parent * animation::ToMatrix(local);

  const animation::Transform result = LocalFromWorld(parent, world);
  CHECK(Near(result.translation, local.translation));
  CHECK(Near(result.scale, local.scale));
  CHECK(std::abs(glm::dot(result.rotation, local.rotation)) ==
        doctest::Approx(1.0f));
}

TEST_CASE("LocalFromWorld keeps a mirror as a negative X scale") {
  const glm::mat4 mirrored =
      glm::scale(glm::mat4(1.0f), glm::vec3(-1.0f, 1.0f, 1.0f));
  const animation::Transform result = LocalFromWorld(glm::mat4(1.0f), mirrored);
  CHECK(result.scale.x == doctest::Approx(-1.0f));
  const glm::mat4 rebuilt = animation::ToMatrix(result);
  for (int c = 0; c < 4; ++c) {
    for (int r = 0; r < 4; ++r) {
      CHECK(rebuilt[c][r] == doctest::Approx(mirrored[c][r]));
    }
  }
}

TEST_CASE("Collider debug draws follow the shape kind") {
  render::DebugLines lines;
  prefab::Physics physics;
  const asset::Bounds bounds{.min = glm::vec3(-1.0f), .max = glm::vec3(1.0f)};

  physics.kind = prefab::ColliderKind::kNone;
  AddCollider(lines, physics, glm::mat4(1.0f), bounds);
  CHECK(lines.empty());

  physics.kind = prefab::ColliderKind::kBox;
  physics.half_extents = {1.0f, 2.0f, 3.0f};
  physics.center = {0.0f, 5.0f, 0.0f};
  AddCollider(lines, physics, glm::mat4(1.0f), bounds);
  REQUIRE(lines.tested().size() == 24);
  for (const render::LineVertex& vertex : lines.tested()) {
    CHECK(std::abs(vertex.position.y - 5.0f) == doctest::Approx(2.0f));
  }

  lines.Clear();
  physics.kind = prefab::ColliderKind::kCapsule;
  physics.center = glm::vec3(0.0f);
  physics.radius = 0.5f;
  physics.half_height = 1.0f;
  AddCollider(lines, physics, glm::mat4(1.0f), bounds);
  CHECK_FALSE(lines.empty());
  for (const render::LineVertex& vertex : lines.tested()) {
    CHECK(std::abs(vertex.position.y) <= 1.5f + 1.0e-4f);
  }

  lines.Clear();
  physics.kind = prefab::ColliderKind::kTriangleMesh;
  AddCollider(lines, physics, glm::mat4(1.0f), bounds);
  CHECK(lines.tested().size() == 24);  // The fallback bounds.
}

TEST_CASE("Socket, light and player start markers are drawn on top") {
  render::DebugLines lines;
  AddSocket(lines, glm::scale(glm::mat4(1.0f), glm::vec3(10.0f)), 0.1f);
  REQUIRE(lines.on_top().size() == 6);
  // The parent's scale does not stretch the socket axes.
  CHECK(glm::length(lines.on_top()[1].position) == doctest::Approx(0.1f));

  lines.Clear();
  AddPointLight(lines, {0.0f, 3.0f, 0.0f}, 5.0f, /*show_range=*/true);
  CHECK_FALSE(lines.on_top().empty());
  CHECK_FALSE(lines.tested().empty());  // The range sphere.

  lines.Clear();
  AddPlayerStart(lines, {0.0f, 0.0f, 0.0f}, 0.0f);
  CHECK_FALSE(lines.on_top().empty());
}

}  // namespace blue::editor
