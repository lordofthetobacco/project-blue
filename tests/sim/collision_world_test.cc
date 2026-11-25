#include "world/core/collision_world.h"

#include <doctest/doctest.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <glm/vec3.hpp>

#include "physics/physics_runtime.h"
#include "physics/physics_world.h"
#include "prefab/prefab.h"
#include "scene/scene.h"
#include "scene/scene_data.h"
#include "tests/scratch_directory.h"
#include "world/core/placement_plan.h"
#include "world/core/prefab_store.h"

namespace blue::world {
namespace {

// A prefab with one node and a unit box collider (half extent 0.5).
prefab::Prefab MakeBox(bool dynamic, std::vector<std::string> children = {}) {
  prefab::Prefab prefab;
  prefab.hierarchy.nodes = {{.name = "root", .parent = -1, .rest = {}}};
  prefab.physics.kind = prefab::ColliderKind::kBox;
  prefab.physics.half_extents = glm::vec3(0.5f);
  prefab.physics.dynamic = dynamic;
  prefab.physics.mass = 4.0f;
  for (std::string& path : children) {
    prefab.children.push_back(
        {.path = std::move(path),
         .node = 0,
         .local = {.translation = glm::vec3(0.0f, 2.0f, 0.0f)}});
  }
  return prefab;
}

scene::Instance MakeInstance(scene::InstanceId id, int prefab,
                             glm::vec3 position) {
  scene::Instance instance;
  instance.id = id;
  instance.prefab = prefab;
  instance.transform.position = position;
  return instance;
}

// The scene shared by the tests: a static wall (id 1, scaled), a crate
// (id 2) with a nested crate, a crate overridden to static (id 3), one with
// physics disabled (id 4) and an empty prefab (id 5).
struct Fixture {
  testing::ScratchDirectory dir{"collision_world"};
  PrefabStore store;
  scene::SceneData scene;

  Fixture() {
    const std::string wall = dir.Child("wall.pbo");
    const std::string crate = dir.Child("crate.pbo");
    const std::string stacked = dir.Child("stacked.pbo");
    const std::string empty = dir.Child("empty.pbo");
    (void)store.Add(MakeBox(false), wall);
    (void)store.Add(MakeBox(true), crate);
    (void)store.Add(MakeBox(true, {crate}), stacked);
    prefab::Prefab nothing;
    nothing.hierarchy.nodes = {{.name = "root", .parent = -1, .rest = {}}};
    (void)store.Add(std::move(nothing), empty);
    scene.prefabs = {wall, stacked, crate, empty};

    scene::Instance scaled = MakeInstance(1, 0, glm::vec3(0.0f, 0.0f, -10.0f));
    scaled.transform.scale = glm::vec3(4.0f, 2.0f, 1.0f);
    scene::Instance pinned = MakeInstance(3, 2, glm::vec3(5.0f, 0.0f, 0.0f));
    pinned.physics = scene::PhysicsOverride{
        .motion = scene::MotionType::kStatic, .friction = 0.9f};
    scene::Instance disabled = MakeInstance(4, 1, glm::vec3(-5.0f, 0.0f, 0.0f));
    disabled.physics = scene::PhysicsOverride{.disabled = true};
    scene.instances = {scaled, MakeInstance(2, 1, glm::vec3(0.0f, 0.0f, 0.0f)),
                       pinned, disabled,
                       MakeInstance(5, 3, glm::vec3(0.0f, 0.0f, 5.0f))};
  }
};

TEST_CASE("CollisionWorld builds one body per collider placement") {
  Fixture fixture;
  const PlacementPlan plan = ResolvePlacements(fixture.scene, fixture.store);
  REQUIRE(plan.issues.empty());
  physics::PhysicsRuntime runtime;
  physics::PhysicsWorld physics(runtime);
  CollisionWorld world(physics);

  const PhysicsBuildSummary summary =
      world.Build(fixture.scene, plan.placements, fixture.store);
  CHECK(summary.created == 4);  // wall, stacked + nested crate, pinned crate
  CHECK(summary.failed == 0);
  CHECK(physics.Stats().bodies == 4);

  SUBCASE("ids, roots and motion") {
    REQUIRE(world.bodies().size() == 4);
    const CollisionBody& wall = world.bodies()[0];
    CHECK(wall.instance == 1);
    CHECK(wall.root);
    CHECK(wall.motion == scene::MotionType::kStatic);
    CHECK_FALSE(physics.IsDynamic(wall.body));

    const CollisionBody& stacked = world.bodies()[1];
    const CollisionBody& nested = world.bodies()[2];
    CHECK(stacked.instance == 2);
    CHECK(stacked.root);
    CHECK(nested.instance == 2);
    CHECK_FALSE(nested.root);
    CHECK(nested.placement == stacked.placement + 1);
    CHECK(physics.IsDynamic(nested.body));
    CHECK(physics.InterpolatedPose(nested.body).position.y ==
          doctest::Approx(2.0f));

    const CollisionBody& pinned = world.bodies()[3];
    CHECK(pinned.instance == 3);
    CHECK(pinned.motion == scene::MotionType::kStatic);
    CHECK_FALSE(pinned.simulated);
    CHECK_FALSE(physics.IsDynamic(pinned.body));

    CHECK(world.InstanceFor(nested.body) == 2);
    CHECK(world.BodyFor(2) == stacked.body);
    CHECK_FALSE(world.BodyFor(4).has_value());
    CHECK_FALSE(world.BodyFor(5).has_value());

    std::vector<CollisionBody> roots;
    world.AppendDynamicRoots(roots);
    REQUIRE(roots.size() == 1);
    CHECK(roots[0].instance == 2);
    CHECK(roots[0].body == stacked.body);
  }

  SUBCASE("scale reaches the shape") {
    const CollisionBody& wall = world.bodies()[0];
    CHECK(wall.scale.x == doctest::Approx(4.0f));
    CHECK(wall.scale.y == doctest::Approx(2.0f));
    CHECK(wall.scale.z == doctest::Approx(1.0f));
    // The wall spans x in [-2, 2]: a ray along -Z at x = 1.9 hits it, one at
    // x = 2.1 passes by.
    const std::optional<physics::RayHit> inside = physics.CastRay(
        glm::vec3(1.9f, 0.0f, -5.0f), glm::vec3(0.0f, 0.0f, -1.0f), 20.0f);
    REQUIRE(inside.has_value());
    CHECK(inside->body == wall.body);
    CHECK(inside->point.z == doctest::Approx(-9.5f));
    CHECK_FALSE(physics
                    .CastRay(glm::vec3(2.1f, 0.0f, -5.0f),
                             glm::vec3(0.0f, 0.0f, -1.0f), 20.0f)
                    .has_value());
    // Height: y in [-1, 1].
    CHECK(physics
              .CastRay(glm::vec3(0.0f, 0.9f, -5.0f),
                       glm::vec3(0.0f, 0.0f, -1.0f), 20.0f)
              .has_value());
  }

  SUBCASE("removing an instance's bodies") {
    CHECK(world.RemoveInstanceBodies(2) == 2);
    CHECK(world.RemoveInstanceBodies(2) == 0);
    CHECK(physics.Stats().bodies == 2);
    CHECK_FALSE(world.BodyFor(2).has_value());
    CHECK_FALSE(world.InstanceFor(world.bodies()[1].body).has_value());
    CHECK(world.bodies()[1].removed);
    std::vector<CollisionBody> roots;
    world.AppendDynamicRoots(roots);
    CHECK(roots.empty());
  }

  SUBCASE("a rebuild replaces every body") {
    const PhysicsBuildSummary again =
        world.Build(fixture.scene, plan.placements, fixture.store);
    CHECK(again.created == 4);
    CHECK(physics.Stats().bodies == 4);
  }
}

TEST_CASE("CollisionWorld can keep dynamic bodies static for clients") {
  Fixture fixture;
  const PlacementPlan plan = ResolvePlacements(fixture.scene, fixture.store);
  physics::PhysicsRuntime runtime;
  physics::PhysicsWorld physics(runtime);
  CollisionWorld world(physics, {.dynamic_bodies = DynamicBodies::kStatic});
  CHECK(world.Build(fixture.scene, plan.placements, fixture.store).created ==
        4);
  for (const CollisionBody& body : world.bodies()) {
    CHECK_FALSE(body.simulated);
    CHECK_FALSE(physics.IsDynamic(body.body));
  }
  // Which scene objects the server simulates is still known.
  std::vector<CollisionBody> roots;
  world.AppendDynamicRoots(roots);
  REQUIRE(roots.size() == 1);
  CHECK(roots[0].instance == 2);
  CHECK(roots[0].motion == scene::MotionType::kDynamic);
}

TEST_CASE("CollisionWorld can make dynamic bodies kinematic for clients") {
  Fixture fixture;
  const PlacementPlan plan = ResolvePlacements(fixture.scene, fixture.store);
  physics::PhysicsRuntime runtime;
  physics::PhysicsWorld simulated(runtime);
  physics::PhysicsWorld kinematic(runtime);
  CollisionWorld server(simulated);
  CollisionWorld client(kinematic,
                        {.dynamic_bodies = DynamicBodies::kKinematic});
  REQUIRE(server.Build(fixture.scene, plan.placements, fixture.store).created ==
          4);
  REQUIRE(client.Build(fixture.scene, plan.placements, fixture.store).created ==
          4);
  for (const CollisionBody& body : client.bodies()) {
    CHECK_FALSE(body.simulated);
  }
  std::vector<CollisionBody> roots;
  client.AppendDynamicRoots(roots);
  REQUIRE(roots.size() == 1);
  const physics::BodyId crate = roots[0].body;

  // Gravity pulls the server's crate down; the client's stays put until it
  // is moved.
  simulated.Update(0.25f);
  kinematic.Update(0.25f);
  CHECK(simulated.InterpolatedPose(crate).position.y < -0.01f);
  CHECK(kinematic.InterpolatedPose(crate).position.y == doctest::Approx(0.0f));
}

TEST_CASE("CollisionWorld counts colliders it cannot restore") {
  testing::ScratchDirectory dir("collision_failed");
  PrefabStore store;
  prefab::Prefab hull;
  hull.hierarchy.nodes = {{.name = "root", .parent = -1, .rest = {}}};
  hull.physics.kind = prefab::ColliderKind::kConvexHull;  // no cooked bytes
  (void)store.Add(std::move(hull), dir.Child("hull.pbo"));
  scene::SceneData scene;
  scene.prefabs = {dir.Child("hull.pbo")};
  scene.instances = {MakeInstance(1, 0, glm::vec3(0.0f)),
                     MakeInstance(2, 0, glm::vec3(1.0f, 0.0f, 0.0f))};
  const PlacementPlan plan = ResolvePlacements(scene, store);

  physics::PhysicsRuntime runtime;
  physics::PhysicsWorld physics(runtime);
  CollisionWorld world(physics);
  const PhysicsBuildSummary summary =
      world.Build(scene, plan.placements, store);
  CHECK(summary.created == 0);
  CHECK(summary.failed == 2);
  CHECK(world.bodies().empty());
}

}  // namespace
}  // namespace blue::world
