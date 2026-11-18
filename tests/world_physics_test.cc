#include "world/world_physics.h"

#include <doctest/doctest.h>

#include <optional>
#include <string>
#include <utility>

#include <glm/vec3.hpp>

#include "physics/physics_runtime.h"
#include "physics/physics_world.h"
#include "physics/shape_cooking.h"
#include "prefab/prefab_file.h"
#include "scene/scene_data.h"
#include "tests/scratch_directory.h"
#include "tests/world_fixtures.h"
#include "world/prefab_cache.h"
#include "world/world.h"

namespace blue::world {
namespace {

scene::InstanceId Place(scene::SceneData& scene, const std::string& path,
                        glm::vec3 position, glm::vec3 scale = glm::vec3(1)) {
  scene::Instance instance;
  instance.prefab = scene::AddPrefab(scene, path);
  instance.transform.position = position;
  instance.transform.scale = scale;
  return scene::AddInstance(scene, std::move(instance));
}

}  // namespace

TEST_CASE("PhysicsWorld creates bodies from cooked shapes with scale") {
  physics::PhysicsRuntime runtime;
  physics::PhysicsWorld physics(runtime);
  auto bytes = physics::CookShape({.kind = physics::CookedShapeKind::kBox,
                                   .half_extents = glm::vec3(0.5f)});
  REQUIRE(bytes.ok());
  auto shape = physics::CookedShape::Restore(bytes.value());
  REQUIRE(shape.ok());

  auto body =
      physics.CreateBody(shape.value(), {.motion = scene::MotionType::kStatic,
                                         .pose = {.position = {0, 0, -10}},
                                         .scale = {4.0f, 1.0f, 1.0f}});
  REQUIRE(body.ok());
  // Scaled to 4 m wide: a ray at x = 1.5 hits it.
  const std::optional<physics::RayHit> hit =
      physics.CastRay({1.5f, 0, 0}, {0, 0, -1}, 100.0f);
  REQUIRE(hit.has_value());
  CHECK(hit->body == body.value());
  CHECK(hit->point.z == doctest::Approx(-9.5f));

  auto bad = physics.CreateBody(
      shape.value(), {.motion = scene::MotionType::kDynamic, .mass = 0.0f});
  REQUIRE_FALSE(bad.ok());
  CHECK(bad.error() == physics::PhysicsError::kInvalidShape);
}

TEST_CASE("WorldPhysics builds bodies, maps hits and syncs dynamic poses") {
  const testing::ScratchDirectory dir("world_physics");
  const std::string box_path = dir.Child("box.pbo");
  REQUIRE(
      prefab::SavePrefab(testing::MakeCubePrefab({"a"}, true), box_path).ok());
  prefab::Prefab dynamic_box = testing::MakeCubePrefab({"a"}, true);
  dynamic_box.physics.dynamic = true;
  dynamic_box.physics.mass = 5.0f;
  const std::string crate_path = dir.Child("crate.pbo");
  REQUIRE(prefab::SavePrefab(dynamic_box, crate_path).ok());
  const std::string plain_path = dir.Child("plain.pbo");
  REQUIRE(prefab::SavePrefab(testing::MakeCubePrefab({"a"}), plain_path).ok());

  scene::SceneData scene;
  const scene::InstanceId ground =
      Place(scene, box_path, {0, -0.5f, 0}, {20, 1, 20});
  const scene::InstanceId crate = Place(scene, crate_path, {0, 5, 0});
  const scene::InstanceId ghost = Place(scene, box_path, {5, 5, 0});
  scene::FindInstance(scene, ghost)->physics =
      scene::PhysicsOverride{.disabled = true};
  Place(scene, plain_path, {9, 0, 0});  // no collider

  testing::FakeGpu gpu;
  PrefabCache cache(gpu);
  World world(cache);
  REQUIRE(world.Build(scene).empty());

  physics::PhysicsRuntime runtime;
  physics::PhysicsWorld physics(runtime);
  WorldPhysics bodies(physics);
  const PhysicsBuildSummary summary = bodies.Build(scene, world);
  CHECK(summary.created == 2);
  CHECK(summary.failed == 0);
  REQUIRE(bodies.BodyFor(crate).has_value());
  CHECK_FALSE(bodies.BodyFor(ghost).has_value());
  CHECK(physics.IsDynamic(*bodies.BodyFor(crate)));
  CHECK_FALSE(physics.IsDynamic(*bodies.BodyFor(ground)));

  // The scaled ground is hit far from its centre; hits map to instances.
  std::optional<physics::RayHit> hit =
      physics.CastRay({8, 3, 8}, {0, -1, 0}, 10.0f);
  REQUIRE(hit.has_value());
  CHECK(bodies.InstanceFor(hit->body) == ground);
  CHECK(hit->point.y == doctest::Approx(0.0f).epsilon(1.0e-3));
  hit = physics.CastRay({0, 10, 0}, {0, -1, 0}, 20.0f);
  REQUIRE(hit.has_value());
  CHECK(bodies.InstanceFor(hit->body) == crate);

  // The crate falls onto the ground; the world follows.
  for (int frame = 0; frame < 180; ++frame) {
    physics.Update(1.0f / 60.0f);
  }
  bodies.SyncToWorld(world);
  const std::optional<asset::Bounds> resting = world.InstanceBounds(crate);
  REQUIRE(resting.has_value());
  CHECK(resting->min.y == doctest::Approx(0.0f).epsilon(0.05));

  // An override can make the crate static.
  scene::FindInstance(scene, crate)->physics =
      scene::PhysicsOverride{.motion = scene::MotionType::kStatic};
  REQUIRE(world.Build(scene).empty());
  CHECK(bodies.Build(scene, world).created == 2);
  CHECK_FALSE(physics.IsDynamic(*bodies.BodyFor(crate)));
}

}  // namespace blue::world
