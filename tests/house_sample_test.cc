// Checks that the sample house used by the default scene imports and becomes
// a static collision mesh, without starting the app.

#include <doctest/doctest.h>

#include <cstddef>

#include <glm/vec3.hpp>

#include "asset/model_importer.h"
#include "core/path.h"
#include "physics/physics_runtime.h"
#include "physics/physics_world.h"
#include "scene/scene.h"
#include "tests/mesh_checks.h"

namespace blue {

TEST_CASE("The sample house imports in metres with its base on the ground") {
  const auto house =
      asset::ImportModel(core::JoinPath(BLUE_SAMPLES_DIR, "house.obj"));
  REQUIRE(house.ok());
  const asset::MeshData& mesh = house.value().mesh;
  CHECK(mesh.indices.size() % 3 == 0);
  CHECK(mesh.indices.size() / 3 > 10000);

  const glm::vec3 size = mesh.bounds.size();
  CHECK(mesh.bounds.min.y == doctest::Approx(0.0f).epsilon(0.01));
  CHECK(size.x == doctest::Approx(6.44f).epsilon(0.01));
  CHECK(size.y == doctest::Approx(7.10f).epsilon(0.01));
  CHECK(size.z == doctest::Approx(14.79f).epsilon(0.01));

  // The MTL file's 124 materials collapse to a few distinct ones.
  const size_t materials = house.value().materials.size();
  MESSAGE("house materials after merging: ", materials);
  CHECK(materials >= 2);
  CHECK(materials < 124);
  testing::CheckSubmeshes(mesh, materials);
  CHECK(mesh.submeshes.size() == materials);
}

TEST_CASE("The sample house becomes a static triangle mesh body") {
  const auto house =
      asset::ImportModel(core::JoinPath(BLUE_SAMPLES_DIR, "house.obj"));
  REQUIRE(house.ok());

  physics::PhysicsRuntime runtime;
  physics::PhysicsWorld world(runtime);
  const auto body = world.CreateBody({
      .body = {.motion = scene::MotionType::kStatic,
               .shape = scene::ShapeType::kTriangleMesh},
      .pose = {.position = {-9.0f, 0.0f, -3.0f}},
      .scale = glm::vec3(1.0f),
      .bounds = house.value().mesh.bounds,
      .mesh = &house.value().mesh,
      .shape_key = "test:house",
  });
  REQUIRE(body.ok());
  CHECK_FALSE(world.IsDynamic(body.value()));

  // A ray dropped onto the middle of the house from above hits its roof.
  const auto hit =
      world.CastRay({-9.0f, 20.0f, -3.0f}, {0.0f, -1.0f, 0.0f}, 30.0f);
  REQUIRE(hit.has_value());
  CHECK(hit->body == body.value());
  CHECK(hit->point.y > 3.0f);
}

}  // namespace blue
