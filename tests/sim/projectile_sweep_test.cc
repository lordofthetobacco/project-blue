#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

#include "asset/mesh_data.h"
#include "core/result.h"
#include "physics/physics_runtime.h"
#include "physics/physics_world.h"
#include "scene/scene.h"
#include "weapon/projectile_system.h"

namespace blue::weapon {
namespace {

constexpr float kSpeed = 948.0f;  // m/s
constexpr float kMass = 0.004f;   // kg
constexpr float kDrag = 1.2e-3f;  // 1/m
const glm::vec3 kGravity(0.0f, -9.81f, 0.0f);
const glm::vec3 kNoGravity(0.0f);
const glm::vec3 kForward(0.0f, 0.0f, -1.0f);

struct Range {
  physics::PhysicsRuntime runtime;
  physics::PhysicsWorld world{runtime};
};

physics::BodyId AddBox(physics::PhysicsWorld& world, asset::Bounds bounds,
                       glm::vec3 position, scene::MotionType motion) {
  physics::BodyDesc desc;
  desc.body.shape = scene::ShapeType::kBox;
  desc.body.motion = motion;
  desc.body.mass = 10.0f;
  desc.pose.position = position;
  desc.bounds = bounds;
  const core::Result<physics::BodyId, physics::PhysicsError> body =
      world.CreateBody(desc);
  REQUIRE(body.ok());
  return body.value();
}

// A static 10 x 10 m wall whose front face is the plane z = `z`.
void AddWall(physics::PhysicsWorld& world, float z) {
  (void)AddBox(
      world,
      asset::Bounds{glm::vec3(-5.0f, -5.0f, 0.0f), glm::vec3(5.0f, 5.0f, 0.0f)},
      glm::vec3(0.0f, 0.0f, z), scene::MotionType::kStatic);
}

void CheckSameProjectiles(std::span<const Projectile> a,
                          std::span<const Projectile> b) {
  REQUIRE(a.size() == b.size());
  for (size_t i = 0; i < a.size(); ++i) {
    CHECK(a[i].position == b[i].position);
    CHECK(a[i].previous_position == b[i].previous_position);
    CHECK(a[i].velocity == b[i].velocity);
    CHECK(a[i].age == b[i].age);
    CHECK(a[i].distance == b[i].distance);
    CHECK(a[i].shot_number == b[i].shot_number);
    CHECK(a[i].id == b[i].id);
  }
}

void CheckSameImpacts(std::span<const Impact> a, std::span<const Impact> b) {
  REQUIRE(a.size() == b.size());
  for (size_t i = 0; i < a.size(); ++i) {
    CHECK(a[i].point == b[i].point);
    CHECK(a[i].normal == b[i].normal);
    CHECK(a[i].velocity == b[i].velocity);
    CHECK(a[i].mass == b[i].mass);
    CHECK(a[i].body == b[i].body);
    CHECK(a[i].flight_time == b[i].flight_time);
    CHECK(a[i].distance == b[i].distance);
    CHECK(a[i].shot_number == b[i].shot_number);
  }
}

// One scenario of tests/projectile_system_test.cc: builds the world, spawns,
// and lists the frame times to run.
struct Scenario {
  const char* name;
  std::function<void(physics::PhysicsWorld&)> build;
  std::function<void(ProjectileSystem&)> spawn;
  glm::vec3 gravity;
  std::array<float, 6> frames;
  ProjectileTuning tuning{};
};

TEST_CASE("Sweep then Resolve({}) matches Update exactly") {
  const std::array<Scenario, 6> scenarios{{
      {"free flight with drag",
       [](physics::PhysicsWorld&) {},
       [](ProjectileSystem& s) {
         s.Spawn(glm::vec3(0.0f),
                 kSpeed * glm::normalize(glm::vec3(0.0f, 0.05f, -1.0f)), kMass,
                 kDrag);
       },
       kGravity,
       {0.0f, 1.0f / 30.0f, 1.0f / 120.0f, 1.0f / 90.0f, 0.1f, 1.0f / 60.0f},
       {}},
      {"wall impact",
       [](physics::PhysicsWorld& w) { AddWall(w, -40.0f); },
       [](ProjectileSystem& s) {
         s.Spawn(glm::vec3(1.0f, 0.5f, 0.0f), kSpeed * kForward, kMass, kDrag,
                 0.0f, 7);
       },
       kGravity,
       {0.0f, 1.0f / 60.0f, 1.0f / 30.0f, 1.0f / 60.0f, 1.0f / 60.0f, 0.0f},
       {}},
      {"dynamic body",
       [](physics::PhysicsWorld& w) {
         w.SetGravity(kNoGravity);
         (void)AddBox(w, asset::Bounds{glm::vec3(-0.5f), glm::vec3(0.5f)},
                      glm::vec3(0.0f, 0.0f, -20.0f),
                      scene::MotionType::kDynamic);
       },
       [](ProjectileSystem& s) {
         s.Spawn(glm::vec3(0.0f), kSpeed * kForward, kMass, kDrag);
         s.Spawn(glm::vec3(0.2f, 0.0f, 0.0f), kSpeed * kForward, kMass, kDrag,
                 0.004f, 2);
       },
       kNoGravity,
       {0.0f, 1.0f / 30.0f, 1.0f / 30.0f, 0.0f, 0.0f, 0.0f},
       {}},
      {"spawn age",
       [](physics::PhysicsWorld& w) { AddWall(w, -20.0f); },
       [](ProjectileSystem& s) {
         s.Spawn(glm::vec3(0.0f), kSpeed * kForward, kMass, 0.0f, 0.015f, 3);
       },
       kNoGravity,
       {1.0f / 60.0f, 1.0f / 60.0f, 1.0f / 60.0f, 0.0f, 0.0f, 0.0f},
       {}},
      {"thin wall at 0.1 s frames",
       [](physics::PhysicsWorld& w) { AddWall(w, -30.0f); },
       [](ProjectileSystem& s) {
         for (int i = 0; i < 4; ++i) {
           s.Spawn(glm::vec3(0.0f, 0.0f, -0.37f * static_cast<float>(i)),
                   kSpeed * kForward, kMass, kDrag, 0.003f * i, i);
         }
       },
       kGravity,
       {0.1f, 0.1f, 0.1f, 0.0f, 0.0f, 0.0f},
       {}},
      {"expiry by age and distance",
       [](physics::PhysicsWorld&) {},
       [](ProjectileSystem& s) {
         s.Spawn(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f), kMass, 0.0f);
         s.Spawn(glm::vec3(0.0f), kSpeed * kForward, kMass, 0.0f);
       },
       kNoGravity,
       {0.0f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f},
       {.max_age = 0.25f, .max_distance = 50.0f}},
  }};
  for (const Scenario& scenario : scenarios) {
    CAPTURE(scenario.name);
    // One runtime may exist at a time; it serves both worlds.
    physics::PhysicsRuntime runtime;
    physics::PhysicsWorld one(runtime);
    physics::PhysicsWorld two(runtime);
    scenario.build(one);
    scenario.build(two);
    ProjectileSystem updated(scenario.tuning);
    ProjectileSystem swept(scenario.tuning);
    scenario.spawn(updated);
    scenario.spawn(swept);
    for (const float dt : scenario.frames) {
      updated.Update(one, scenario.gravity, dt);
      swept.Sweep(two, scenario.gravity, dt);
      swept.Resolve(two, {});
      CheckSameProjectiles(updated.projectiles(), swept.projectiles());
      CheckSameImpacts(updated.impacts(), swept.impacts());
      CHECK(swept.player_impacts().empty());
      one.Update(dt);
      two.Update(dt);
    }
  }
}

// Returns the index of the segment of `projectile` that crosses the plane
// z = `z` and the fraction at which it does.
std::optional<PlayerHit> HitAt(const ProjectileSystem& system, int projectile,
                               float z, uint32_t victim) {
  const std::span<const Segment> segments = system.segments();
  for (size_t i = 0; i < segments.size(); ++i) {
    const Segment& s = segments[i];
    if (s.projectile != projectile || s.delta.z == 0.0f) {
      continue;
    }
    const float fraction = (z - s.origin.z) / s.delta.z;
    if (fraction >= 0.0f && fraction <= 1.0f) {
      return PlayerHit{.segment = static_cast<int>(i),
                       .victim = victim,
                       .fraction = fraction};
    }
  }
  return std::nullopt;
}

// A system with one bullet fired straight ahead along -z from the origin,
// already started (age 0), owner 77 and shot 5.
ProjectileSystem OneBullet(Range& range) {
  ProjectileSystem system;
  system.Spawn(glm::vec3(0.0f), kSpeed * kForward, kMass, 0.0f, 0.0f, 5, 77);
  system.Update(range.world, kNoGravity, 0.0f);
  return system;
}

TEST_CASE("Sweep records the substeps without moving any bullet") {
  Range range;
  ProjectileSystem system = OneBullet(range);
  const Projectile before = system.projectiles()[0];
  system.Sweep(range.world, kNoGravity, 1.0f / 60.0f);
  REQUIRE(system.segments().size() == 4);  // 1/60 s in 1/240 s substeps
  CHECK(system.projectiles()[0].position == before.position);
  glm::vec3 expected_origin = before.position;
  for (const Segment& segment : system.segments()) {
    CHECK(segment.projectile == before.id);
    CHECK(segment.owner == 77u);
    CHECK(segment.shot_number == 5);
    CHECK(segment.mass == kMass);
    CHECK(segment.world_fraction == 1.0f);
    CHECK(segment.origin == expected_origin);
    expected_origin += segment.delta;
  }
  system.Resolve(range.world, {});
  CHECK(system.projectiles()[0].position == expected_origin);
  CHECK(system.segments().empty());
}

TEST_CASE("a player hit before the wall wins and suppresses the wall impact") {
  Range range;
  AddWall(range.world, -40.0f);
  ProjectileSystem system = OneBullet(range);
  const int id = system.projectiles()[0].id;
  system.Sweep(range.world, kNoGravity, 0.05f);  // 47 m: reaches the wall
  REQUIRE(system.segments().back().world_fraction < 1.0f);
  const std::optional<PlayerHit> hit = HitAt(system, id, -20.0f, 9);
  REQUIRE(hit.has_value());
  system.Resolve(range.world, std::span<const PlayerHit>(&*hit, 1));
  CHECK(system.impacts().empty());
  CHECK(system.projectiles().empty());
  REQUIRE(system.player_impacts().size() == 1);
  const PlayerImpact& impact = system.player_impacts()[0];
  CHECK(impact.victim == 9u);
  CHECK(impact.owner == 77u);
  CHECK(impact.shot_number == 5);
  CHECK(impact.mass == kMass);
  CHECK(impact.point.z == doctest::Approx(-20.0f).epsilon(1.0e-5));
  CHECK(impact.distance == doctest::Approx(20.0f).epsilon(1.0e-4));
  CHECK(impact.flight_time == doctest::Approx(20.0f / kSpeed).epsilon(1.0e-4));
  CHECK(-impact.velocity.z == doctest::Approx(kSpeed));
}

TEST_CASE("a player hit behind the wall is ignored") {
  Range range;
  AddWall(range.world, -40.0f);
  ProjectileSystem system = OneBullet(range);
  system.Sweep(range.world, kNoGravity, 0.05f);
  const auto last = static_cast<int>(system.segments().size()) - 1;
  const float wall = system.segments().back().world_fraction;
  REQUIRE(wall < 1.0f);
  // Behind the wall on the wall's own segment.
  const PlayerHit hit{
      .segment = last, .victim = 9, .fraction = 0.5f * (wall + 1.0f)};
  system.Resolve(range.world, std::span<const PlayerHit>(&hit, 1));
  CHECK(system.player_impacts().empty());
  REQUIRE(system.impacts().size() == 1);
  CHECK(system.impacts()[0].point.z == doctest::Approx(-40.0f).epsilon(1.0e-4));
}

TEST_CASE("the earliest player hit of a bullet wins") {
  Range range;
  ProjectileSystem system = OneBullet(range);
  const int id = system.projectiles()[0].id;
  system.Sweep(range.world, kNoGravity, 0.05f);
  const std::optional<PlayerHit> far = HitAt(system, id, -30.0f, 1);
  const std::optional<PlayerHit> near = HitAt(system, id, -10.0f, 2);
  REQUIRE(far.has_value());
  REQUIRE(near.has_value());
  PlayerHit same_segment_later = *near;
  same_segment_later.victim = 3;
  same_segment_later.fraction = std::min(near->fraction + 0.1f, 1.0f);
  const std::array<PlayerHit, 4> hits{
      *far, same_segment_later, *near,
      PlayerHit{.segment = 1000, .victim = 4}};  // no such segment
  system.Resolve(range.world, hits);
  REQUIRE(system.player_impacts().size() == 1);
  CHECK(system.player_impacts()[0].victim == 2u);
  CHECK(system.projectiles().empty());
}

TEST_CASE("two bullets are resolved independently") {
  Range range;
  AddWall(range.world, -40.0f);
  ProjectileSystem system;
  system.Spawn(glm::vec3(-1.0f, 0.0f, 0.0f), kSpeed * kForward, kMass, 0.0f,
               0.0f, 1, 10);
  system.Spawn(glm::vec3(1.0f, 0.0f, 0.0f), kSpeed * kForward, kMass, 0.0f,
               0.0f, 2, 20);
  system.Spawn(glm::vec3(0.0f, 2.0f, 0.0f), 0.5f * kSpeed * kForward, kMass,
               0.0f, 0.0f, 3, 30);
  system.Update(range.world, kNoGravity, 0.0f);
  const int first = system.projectiles()[0].id;
  const int second = system.projectiles()[1].id;
  CHECK(first != second);
  system.Sweep(range.world, kNoGravity, 0.05f);
  const std::optional<PlayerHit> hit = HitAt(system, first, -25.0f, 9);
  REQUIRE(hit.has_value());
  system.Resolve(range.world, std::span<const PlayerHit>(&*hit, 1));
  REQUIRE(system.player_impacts().size() == 1);
  CHECK(system.player_impacts()[0].owner == 10u);
  REQUIRE(system.impacts().size() == 1);  // the second bullet hit the wall
  CHECK(system.impacts()[0].shot_number == 2);
  REQUIRE(system.projectiles().size() == 1);  // the slow third flies on
  CHECK(system.projectiles()[0].shot_number == 3);
  CHECK(system.projectiles()[0].owner == 30u);
}

TEST_CASE("sweeps are deterministic") {
  Range range;
  AddWall(range.world, -60.0f);
  ProjectileSystem a;
  ProjectileSystem b;
  for (ProjectileSystem* system : {&a, &b}) {
    for (int i = 0; i < 5; ++i) {
      system->Spawn(glm::vec3(0.3f * static_cast<float>(i), 0.0f, 0.0f),
                    kSpeed * glm::normalize(glm::vec3(0.01f * i, 0.02f, -1.0f)),
                    kMass, kDrag, 0.002f * i, i,
                    100u + static_cast<uint32_t>(i));
    }
  }
  for (int frame = 0; frame < 6; ++frame) {
    a.Sweep(range.world, kGravity, 1.0f / 60.0f);
    b.Sweep(range.world, kGravity, 1.0f / 60.0f);
    REQUIRE(a.segments().size() == b.segments().size());
    for (size_t i = 0; i < a.segments().size(); ++i) {
      const Segment& x = a.segments()[i];
      const Segment& y = b.segments()[i];
      CHECK(x.projectile == y.projectile);
      CHECK(x.shot_number == y.shot_number);
      CHECK(x.owner == y.owner);
      CHECK(x.origin == y.origin);
      CHECK(x.delta == y.delta);
      CHECK(x.world_fraction == y.world_fraction);
      CHECK(x.velocity == y.velocity);
      CHECK(x.mass == y.mass);
      CHECK(x.world_body == y.world_body);
      CHECK(x.world_normal == y.world_normal);
    }
    a.Resolve(range.world, {});
    b.Resolve(range.world, {});
  }
}

TEST_CASE("a Spawn between Sweep and Resolve discards the sweep") {
  Range range;
  ProjectileSystem system = OneBullet(range);
  const glm::vec3 before = system.projectiles()[0].position;
  system.Sweep(range.world, kNoGravity, 1.0f / 60.0f);
  system.Spawn(glm::vec3(5.0f), kSpeed * kForward, kMass, 0.0f);
  CHECK(system.segments().empty());
  system.Resolve(range.world, {});
  REQUIRE(system.projectiles().size() == 2);
  CHECK(system.projectiles()[0].position == before);
}

}  // namespace
}  // namespace blue::weapon
