#include <doctest/doctest.h>

#include <cmath>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include "physics/physics_runtime.h"
#include "physics/physics_world.h"
#include "scene/mscn_serializer.h"
#include "scene/scene_data.h"
#include "tests/world_fixtures.h"
#include "world/core/collision_world.h"
#include "world/core/placement.h"
#include "world/core/placement_plan.h"
#include "world/core/prefab_store.h"
#include "world/prefab_cache.h"
#include "world/world.h"
#include "world/world_physics.h"

// The runtime world (World + WorldPhysics over a PrefabCache) and the
// headless one the dedicated server builds (PrefabStore + ResolvePlacements
// + CollisionWorld) must agree bit for bit on the cooked sample range.

namespace blue::world {
namespace {

bool SameMatrix(const glm::mat4& a, const glm::mat4& b) {
  for (int column = 0; column < 4; ++column) {
    for (int row = 0; row < 4; ++row) {
      if (a[column][row] != b[column][row]) {
        return false;
      }
    }
  }
  return true;
}

// The rays of the fan: from each origin, 256 / origins.size() directions
// spread over the sphere (a golden-angle spiral, so the fan is fixed).
std::vector<glm::vec3> RayDirections(size_t count) {
  std::vector<glm::vec3> directions;
  directions.reserve(count);
  constexpr float kGoldenAngle = 2.39996323f;
  for (size_t i = 0; i < count; ++i) {
    const float y = 1.0f - 2.0f * (static_cast<float>(i) + 0.5f) /
                               static_cast<float>(count);
    const float radius = std::sqrt(1.0f - y * y);
    const float angle = kGoldenAngle * static_cast<float>(i);
    directions.emplace_back(radius * std::cos(angle), y,
                            radius * std::sin(angle));
  }
  return directions;
}

TEST_CASE("The headless world builds the same collision as the runtime one") {
  auto loaded = scene::LoadMscn(BLUE_ASSETS_DIR "/range.mscn");
  REQUIRE(loaded.ok());
  const scene::SceneData& scene = loaded.value();
  physics::PhysicsRuntime runtime;

  // The runtime path.
  testing::FakeGpu gpu;
  PrefabCache cache(gpu);
  World world(cache);
  CHECK(world.Build(scene).empty());
  physics::PhysicsWorld runtime_physics(runtime);
  WorldPhysics runtime_bodies(runtime_physics);
  const PhysicsBuildSummary runtime_summary =
      runtime_bodies.Build(scene, world);

  // The headless path.
  PrefabStore store;
  const PlacementPlan plan = ResolvePlacements(scene, store);
  CHECK(plan.issues.empty());
  physics::PhysicsWorld headless_physics(runtime);
  CollisionWorld headless_bodies(headless_physics);
  const PhysicsBuildSummary headless_summary =
      headless_bodies.Build(scene, plan.placements, store);

  SUBCASE("placements") {
    const std::span<const Placement> expected = world.placements();
    REQUIRE(plan.placements.size() == expected.size());
    CHECK(expected.size() >= scene.instances.size());
    for (size_t i = 0; i < expected.size(); ++i) {
      CAPTURE(i);
      const Placement& a = expected[i];
      const Placement& b = plan.placements[i];
      CHECK(a.owner == b.owner);
      CHECK(cache.key(a.prefab) == store.key(b.prefab));
      CHECK(a.parent == b.parent);
      CHECK(a.depth == b.depth);
      CHECK(SameMatrix(a.world, b.world));
    }
  }

  SUBCASE("bodies") {
    CHECK(runtime_summary.created == headless_summary.created);
    CHECK(runtime_summary.failed == headless_summary.failed);
    CHECK(headless_summary.created >= 9);
    CHECK(runtime_physics.Stats().bodies == headless_physics.Stats().bodies);
    const std::span<const CollisionBody> bodies = headless_bodies.bodies();
    REQUIRE(bodies.size() == static_cast<size_t>(headless_summary.created));
    for (const CollisionBody& body : bodies) {
      CAPTURE(body.instance);
      // Bodies are created in the same order, so they get the same ids.
      CHECK(runtime_bodies.InstanceFor(body.body) == body.instance);
      if (body.root) {
        CHECK(runtime_bodies.BodyFor(body.instance) == body.body);
      }
      const physics::Pose a = runtime_physics.InterpolatedPose(body.body);
      const physics::Pose b = headless_physics.InterpolatedPose(body.body);
      CHECK(a.position == b.position);
      CHECK(a.rotation == b.rotation);
      CHECK(runtime_physics.IsDynamic(body.body) ==
            headless_physics.IsDynamic(body.body));
      CHECK(body.simulated == headless_physics.IsDynamic(body.body));
      const glm::mat4& placed = plan.placements[body.placement].world;
      CHECK(body.scale.x == doctest::Approx(glm::length(glm::vec3(placed[0]))));
    }
  }

  SUBCASE("ray casts") {
    const glm::vec3 start = scene.player_start.position;
    const std::vector<glm::vec3> origins = {
        start,
        start + glm::vec3(0.0f, 1.6f, 0.0f),
        glm::vec3(0.0f, 2.0f, 0.0f),
        glm::vec3(10.0f, 5.0f, -20.0f),
    };
    const std::vector<glm::vec3> directions = RayDirections(256 / 4);
    int hits = 0;
    for (const glm::vec3& origin : origins) {
      for (const glm::vec3& direction : directions) {
        CAPTURE(origin);
        CAPTURE(direction);
        const std::optional<physics::RayHit> a =
            runtime_physics.CastRay(origin, direction, 500.0f);
        const std::optional<physics::RayHit> b =
            headless_physics.CastRay(origin, direction, 500.0f);
        REQUIRE(a.has_value() == b.has_value());
        if (!a) {
          continue;
        }
        ++hits;
        CHECK(a->body == b->body);
        CHECK(a->point == b->point);
        CHECK(a->normal == b->normal);
        CHECK(a->fraction == b->fraction);
      }
    }
    // The fan must actually test something.
    CHECK(hits > 64);
  }
}

}  // namespace
}  // namespace blue::world
