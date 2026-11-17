#include "weapon/projectile_system.h"

#include <doctest/doctest.h>

#include <cmath>
#include <memory>

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

#include "asset/mesh_data.h"
#include "core/result.h"
#include "physics/character.h"
#include "physics/physics_runtime.h"
#include "physics/physics_world.h"
#include "scene/scene.h"

namespace blue::weapon {
namespace {

constexpr float kSpeed = 948.0f;  // m/s
constexpr float kMass = 0.004f;   // kg
constexpr float kDrag = 1.2e-3f;  // 1/m
constexpr float kG = 9.81f;
const glm::vec3 kGravity(0.0f, -kG, 0.0f);
const glm::vec3 kNoGravity(0.0f);
const glm::vec3 kForward(0.0f, 0.0f, -1.0f);

// A world plus the runtime it needs.
struct Range {
  physics::PhysicsRuntime runtime;
  physics::PhysicsWorld world{runtime};
};

// Adds a box with local `bounds` at `position`; returns its handle.
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

// A static 10 x 10 m wall whose front face is the plane z = `z`; zero depth,
// so the world thickens it to 0.1 m behind that face.
physics::BodyId AddWall(physics::PhysicsWorld& world, float z) {
  return AddBox(
      world,
      asset::Bounds{glm::vec3(-5.0f, -5.0f, 0.0f), glm::vec3(5.0f, 5.0f, 0.0f)},
      glm::vec3(0.0f, 0.0f, z), scene::MotionType::kStatic);
}

// Steps `system` `count` times by `dt`.
void Run(ProjectileSystem& system, physics::PhysicsWorld& world,
         glm::vec3 gravity, float dt, int count) {
  for (int i = 0; i < count; ++i) {
    system.Update(world, gravity, dt);
  }
}

TEST_CASE("drop without drag matches the analytic value over 100 m") {
  Range range;
  ProjectileSystem system;
  system.Spawn(glm::vec3(0.0f), kSpeed * kForward, kMass, 0.0f);
  system.Update(range.world, kGravity, 0.0f);  // the fresh bullet, age 0
  const float flight = 100.0f / kSpeed;
  Run(system, range.world, kGravity, flight / 7.0f, 7);
  REQUIRE(system.projectiles().size() == 1);
  const Projectile& bullet = system.projectiles()[0];
  CHECK(bullet.position.z == doctest::Approx(-100.0f).epsilon(1.0e-5));
  CHECK(-bullet.position.y ==
        doctest::Approx(0.5f * kG * flight * flight).epsilon(1.0e-3));
  CHECK(bullet.velocity.y == doctest::Approx(-kG * flight).epsilon(1.0e-3));
  CHECK(bullet.age == doctest::Approx(flight));
  CHECK(bullet.distance == doctest::Approx(100.0f).epsilon(1.0e-4));
}

TEST_CASE("trajectories do not depend on the frame rate") {
  Range range;
  ProjectileSystem coarse;
  ProjectileSystem fine;
  ProjectileSystem odd;
  for (ProjectileSystem* system : {&coarse, &fine, &odd}) {
    system->Spawn(glm::vec3(0.0f),
                  kSpeed * glm::normalize(glm::vec3(0.0f, 0.05f, -1.0f)), kMass,
                  kDrag);
    system->Update(range.world, kGravity, 0.0f);
  }
  Run(coarse, range.world, kGravity, 1.0f / 30.0f, 15);
  Run(fine, range.world, kGravity, 1.0f / 120.0f, 60);
  Run(odd, range.world, kGravity, 1.0f / 90.0f, 45);
  const glm::vec3 reference = coarse.projectiles()[0].position;
  CHECK(glm::length(fine.projectiles()[0].position - reference) < 2.0e-3f);
  CHECK(glm::length(odd.projectiles()[0].position - reference) < 2.0e-3f);
  CHECK(-reference.z > 300.0f);  // it really flew, ~0.5 s
}

TEST_CASE("drag slows the bullet like the analytic v0 / (1 + k v0 t)") {
  Range range;
  ProjectileSystem system;
  system.Spawn(glm::vec3(0.0f), kSpeed * kForward, kMass, kDrag);
  system.Update(range.world, kNoGravity, 0.0f);
  Run(system, range.world, kNoGravity, 0.05f, 10);
  REQUIRE(system.projectiles().size() == 1);
  const float expected = kSpeed / (1.0f + kDrag * kSpeed * 0.5f);
  CHECK(glm::length(system.projectiles()[0].velocity) ==
        doctest::Approx(expected).epsilon(2.0e-3));
  // 948 -> ~604 m/s after 0.5 s: about what an M855 keeps at 400 m.
  CHECK(expected < 650.0f);
}

TEST_CASE("a wall stops the bullet and reports the impact") {
  Range range;
  const physics::BodyId wall = AddWall(range.world, -40.0f);
  ProjectileSystem system;
  system.Spawn(glm::vec3(1.0f, 0.5f, 0.0f), kSpeed * kForward, kMass, kDrag,
               0.0f, 7);
  system.Update(range.world, kGravity, 0.0f);
  system.Update(range.world, kGravity, 1.0f / 60.0f);
  CHECK(system.impacts().empty());  // 15.8 m: not there yet
  system.Update(range.world, kGravity, 1.0f / 30.0f);
  CHECK(system.projectiles().empty());
  REQUIRE(system.impacts().size() == 1);
  const Impact& impact = system.impacts()[0];
  CHECK(impact.body == wall);
  CHECK(impact.point.z == doctest::Approx(-40.0f).epsilon(1.0e-4));
  CHECK(impact.point.x == doctest::Approx(1.0f).epsilon(1.0e-4));
  CHECK(impact.normal.z == doctest::Approx(1.0f).epsilon(1.0e-3));
  CHECK(impact.mass == kMass);
  CHECK(-impact.velocity.z > 900.0f);
  CHECK(impact.shot_number == 7);
  CHECK(impact.distance == doctest::Approx(40.0f).epsilon(1.0e-4));
  // 40 m at ~946 m/s average.
  CHECK(impact.flight_time == doctest::Approx(40.0f / 946.0f).epsilon(2.0e-3));
  // Impacts belong to the most recent Update only.
  system.Update(range.world, kGravity, 1.0f / 60.0f);
  CHECK(system.impacts().empty());
}

TEST_CASE("a dynamic body gains the bullet's momentum") {
  Range range;
  range.world.SetGravity(kNoGravity);
  const physics::BodyId box =
      AddBox(range.world, asset::Bounds{glm::vec3(-0.5f), glm::vec3(0.5f)},
             glm::vec3(0.0f, 0.0f, -20.0f), scene::MotionType::kDynamic);
  ProjectileSystem system;
  system.Spawn(glm::vec3(0.0f), kSpeed * kForward, kMass, kDrag);
  system.Update(range.world, kNoGravity, 0.0f);
  system.Update(range.world, kNoGravity, 1.0f / 30.0f);
  REQUIRE(system.impacts().size() == 1);
  const Impact& impact = system.impacts()[0];
  CHECK(impact.body == box);
  const glm::vec3 momentum = 10.0f * range.world.LinearVelocity(box);
  const glm::vec3 expected = kMass * impact.velocity;
  CHECK(momentum.z == doctest::Approx(expected.z).epsilon(0.01));
  CHECK(std::abs(momentum.x) < 1.0e-3f);
  CHECK(-momentum.z > 3.5f);  // ~4 g at ~930 m/s
}

TEST_CASE("bullets never tunnel through a thin wall") {
  Range range;
  AddWall(range.world, -30.0f);
  ProjectileSystem system;
  // Frame times up to 0.1 s move the bullet ~95 m per frame.
  int hits = 0;
  for (const float dt : {1.0f / 30.0f, 1.0f / 60.0f, 1.0f / 144.0f, 0.1f}) {
    for (int offset = 0; offset < 8; ++offset) {
      system.Clear();
      system.Spawn(glm::vec3(0.0f, 0.0f, -0.37f * static_cast<float>(offset)),
                   kSpeed * kForward, kMass, kDrag, 0.003f * offset);
      for (int i = 0; i < 10 && !system.projectiles().empty(); ++i) {
        system.Update(range.world, kGravity, dt);
        hits += static_cast<int>(system.impacts().size());
      }
    }
  }
  CHECK(hits == 32);
}

TEST_CASE("an impact's flight time includes the spawn age") {
  Range range;
  AddWall(range.world, -20.0f);
  ProjectileSystem system;
  // 15 ms of the flight happened before this frame's end: 14.2 m.
  system.Spawn(glm::vec3(0.0f), kSpeed * kForward, kMass, 0.0f, 0.015f, 3);
  system.Update(range.world, kNoGravity, 1.0f / 60.0f);
  CHECK(system.impacts().empty());
  CHECK(system.projectiles()[0].shot_number == 3);
  system.Update(range.world, kNoGravity, 1.0f / 60.0f);
  REQUIRE(system.impacts().size() == 1);
  const Impact& impact = system.impacts()[0];
  CHECK(impact.flight_time == doctest::Approx(20.0f / kSpeed).epsilon(1.0e-3));
  CHECK(impact.distance == doctest::Approx(20.0f).epsilon(1.0e-4));
  CHECK(impact.shot_number == 3);
}

TEST_CASE("Spawn(age) pre-advances the bullet by its age") {
  Range range;
  ProjectileSystem system;
  system.Spawn(glm::vec3(0.0f), kSpeed * kForward, kMass, 0.0f, 0.01f);
  REQUIRE(system.projectiles().size() == 1);
  CHECK(system.projectiles()[0].position.z == 0.0f);
  // The fresh bullet advances by its age only, not by this dt.
  system.Update(range.world, kNoGravity, 1.0f);
  REQUIRE(system.projectiles().size() == 1);
  const Projectile& bullet = system.projectiles()[0];
  CHECK(bullet.position.z == doctest::Approx(-9.48f).epsilon(1.0e-4));
  CHECK(bullet.previous_position.z == 0.0f);
  CHECK(bullet.age == doctest::Approx(0.01f));
  system.Update(range.world, kNoGravity, 0.01f);
  CHECK(system.projectiles()[0].position.z ==
        doctest::Approx(-18.96f).epsilon(1.0e-4));
  CHECK(system.projectiles()[0].previous_position.z ==
        doctest::Approx(-9.48f).epsilon(1.0e-4));
}

TEST_CASE("bullets expire by age and by distance") {
  Range range;
  ProjectileTuning tuning;
  tuning.max_age = 0.5f;
  tuning.max_distance = 1.0e6f;
  ProjectileSystem by_age(tuning);
  by_age.Spawn(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f), kMass, 0.0f);
  by_age.Update(range.world, kNoGravity, 0.0f);
  Run(by_age, range.world, kNoGravity, 0.1f, 4);
  CHECK(by_age.projectiles().size() == 1);
  Run(by_age, range.world, kNoGravity, 0.1f, 2);
  CHECK(by_age.projectiles().empty());
  CHECK(by_age.impacts().empty());

  tuning.max_age = 100.0f;
  tuning.max_distance = 50.0f;
  ProjectileSystem by_distance(tuning);
  by_distance.Spawn(glm::vec3(0.0f), kSpeed * kForward, kMass, 0.0f);
  by_distance.Update(range.world, kNoGravity, 0.0f);
  by_distance.Update(range.world, kNoGravity, 1.0f / 30.0f);  // 31.6 m
  CHECK(by_distance.projectiles().size() == 1);
  by_distance.Update(range.world, kNoGravity, 1.0f / 30.0f);
  CHECK(by_distance.projectiles().empty());
}

TEST_CASE("the count cap drops the oldest bullet and storage never grows") {
  Range range;
  ProjectileTuning tuning;
  tuning.capacity = 8;
  ProjectileSystem system(tuning);
  system.Spawn(glm::vec3(0.0f), kForward, kMass, 0.0f);
  const Projectile* storage = system.projectiles().data();
  for (int i = 1; i <= 20; ++i) {
    system.Spawn(glm::vec3(static_cast<float>(i), 0.0f, 0.0f), kForward, kMass,
                 0.0f);
    system.Update(range.world, kGravity, 1.0f / 60.0f);
  }
  REQUIRE(system.projectiles().size() == 8);
  CHECK(system.projectiles().data() == storage);
  CHECK(system.projectiles()[0].position.x == 13.0f);  // oldest kept
  CHECK(system.projectiles()[7].position.x == 20.0f);
  system.Clear();
  CHECK(system.projectiles().empty());
}

TEST_CASE("bullets pass through the player's character capsule") {
  Range range;
  auto character = physics::Character::Create(range.world, {},
                                              glm::vec3(0.0f, 0.0f, -10.0f));
  REQUIRE(character.ok());
  range.world.Update(1.0f / 60.0f);
  ProjectileSystem system;
  system.Spawn(glm::vec3(0.0f, 1.0f, 0.0f), kSpeed * kForward, kMass, kDrag);
  system.Update(range.world, kNoGravity, 0.0f);
  system.Update(range.world, kNoGravity, 1.0f / 30.0f);
  CHECK(system.impacts().empty());
  CHECK(system.projectiles().size() == 1);
}

}  // namespace
}  // namespace blue::weapon
