#include <doctest/doctest.h>

#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include "physics/physics_runtime.h"
#include "physics/physics_world.h"
#include "render/scene_view.h"
#include "scene/mscn_serializer.h"
#include "scene/scene_data.h"
#include "tests/world_fixtures.h"
#include "world/prefab_cache.h"
#include "world/world.h"
#include "world/world_physics.h"

namespace blue::world {

TEST_CASE("The cooked sample range builds into a world with bodies") {
  auto scene = scene::LoadMscn(BLUE_ASSETS_DIR "/range.mscn");
  REQUIRE(scene.ok());
  testing::FakeGpu gpu;
  PrefabCache cache(gpu);
  World world(cache);

  const auto start = std::chrono::steady_clock::now();
  const std::vector<WorldIssue> issues = world.Build(scene.value());
  const double first_ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - start)
                              .count();
  for (const WorldIssue& issue : issues) {
    MESSAGE("issue: instance ", issue.instance, ": ", issue.message);
  }
  CHECK(issues.empty());
  CHECK(scene::ValidateAgainstPrefabs(scene.value(),
                                      world.MaterialNames(scene.value()))
            .empty());

  std::vector<render::DrawItem> draws;
  world.AppendDraws(draws);
  CHECK(draws.size() >= scene.value().instances.size());
  CHECK(world.point_lights().size() >= 1);

  // Every target is pickable from the firing position.
  const glm::vec3 eye = scene.value().player_start.position;
  int targets = 0;
  for (const scene::Instance& instance : scene.value().instances) {
    if (!instance.name.starts_with("target ")) {
      continue;
    }
    ++targets;
    const std::optional<asset::Bounds> bounds =
        world.InstanceBounds(instance.id);
    REQUIRE(bounds.has_value());
    const glm::vec3 aim = bounds->center() - eye;
    const std::optional<PickHit> hit =
        world.Pick(eye, aim, 1000.0f, PickMode::kTriangles);
    REQUIRE(hit.has_value());
    CHECK(hit->instance == instance.id);
  }
  CHECK(targets == 7);

  physics::PhysicsRuntime runtime;
  physics::PhysicsWorld physics(runtime);
  WorldPhysics bodies(physics);
  const PhysicsBuildSummary summary = bodies.Build(scene.value(), world);
  CHECK(summary.failed == 0);
  CHECK(summary.created >= 9);  // ground, house, crate, 7 targets...

  MESSAGE("range.mscn: first build (load + fake upload) ", first_ms, " ms, ",
          world.placements().size(), " placements, ", draws.size(), " draws, ",
          summary.created, " bodies");
}

}  // namespace blue::world
