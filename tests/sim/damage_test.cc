#include "sim/damage.h"

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

#include "physics/physics_runtime.h"
#include "physics/physics_world.h"
#include "scene/scene.h"
#include "scene/scene_data.h"
#include "sim/player_state.h"
#include "sim/sim_world.h"
#include "sim/user_command.h"
#include "tests/sim/sim_test_scene.h"

namespace blue::sim {
namespace {

constexpr scene::PlayerStart kStart{.position = glm::vec3(0.0f, 1.65f, 0.0f)};

physics::BodyDesc StaticBox(glm::vec3 center, glm::vec3 half_extents) {
  physics::BodyDesc desc;
  desc.body.motion = scene::MotionType::kStatic;
  desc.pose.position = center;
  desc.bounds = {.min = -half_extents, .max = half_extents};
  return desc;
}

float HorizontalDistance(glm::vec3 a, glm::vec3 b) {
  return glm::length(glm::vec3(a.x - b.x, 0.0f, a.z - b.z));
}

TEST_CASE("Hit zones split the capsule height") {
  CHECK(ZoneOfHeight(1.0f) == HitZone::kHead);
  CHECK(ZoneOfHeight(0.86f) == HitZone::kHead);
  CHECK(ZoneOfHeight(0.84f) == HitZone::kTorso);
  CHECK(ZoneOfHeight(0.45f) == HitZone::kTorso);
  CHECK(ZoneOfHeight(0.44f) == HitZone::kLegs);
  CHECK(ZoneOfHeight(0.0f) == HitZone::kLegs);
  CHECK(ZoneOfHeight(-1.0f) == HitZone::kLegs);
  CHECK(ZoneOfHeight(2.0f) == HitZone::kHead);
  CHECK(ZoneOfHeight(std::numeric_limits<float>::quiet_NaN()) ==
        HitZone::kTorso);
  CHECK(ZoneMultiplier(HitZone::kHead) == 4.0f);
  CHECK(ZoneMultiplier(HitZone::kTorso) == 1.0f);
  CHECK(ZoneMultiplier(HitZone::kLegs) == 0.6f);
}

TEST_CASE("Damage is k times the relative momentum times the zone") {
  const glm::vec3 bullet(0.0f, 0.0f, -948.0f);
  const glm::vec3 still(0.0f);
  // 9 * 0.004 * 948 = 34.128.
  CHECK(ComputeDamage(0.004f, bullet, still, HitZone::kTorso) == 34);
  CHECK(ComputeDamage(0.004f, bullet, still, HitZone::kHead) == 137);
  CHECK(ComputeDamage(0.004f, bullet, still, HitZone::kLegs) == 20);
  // A target running away takes less.
  CHECK(ComputeDamage(0.004f, bullet, glm::vec3(0.0f, 0.0f, -100.0f),
                      HitZone::kTorso) == 31);
  CHECK(kDamagePerMomentum == 9.0f);
  // Anything that hits does at least a point; nothing does nothing.
  CHECK(ComputeDamage(0.0001f, glm::vec3(0.0f, 0.0f, -1.0f), still,
                      HitZone::kLegs) == 1);
  CHECK(ComputeDamage(0.004f, still, still, HitZone::kTorso) == 0);
  CHECK(ComputeDamage(-1.0f, bullet, still, HitZone::kTorso) == 0);
  CHECK(ComputeDamage(std::numeric_limits<float>::infinity(), bullet, still,
                      HitZone::kTorso) == 0);
}

TEST_CASE("Spawn points lie on the ring, on the floor, deterministically") {
  physics::PhysicsRuntime runtime;
  physics::PhysicsWorld world(runtime, {.worker_threads = 0});
  REQUIRE(world
              .CreateBody(StaticBox(glm::vec3(0.0f, -0.5f, 0.0f),
                                    glm::vec3(20.0f, 0.5f, 20.0f)))
              .ok());
  const SpawnRing ring;
  const glm::vec3 a = ChooseSpawnPoint(world, kStart, 1, 0, {}, 1.65f);
  CHECK(a == ChooseSpawnPoint(world, kStart, 1, 0, {}, 1.65f));
  CHECK(a.y == doctest::Approx(0.0f).epsilon(1e-4));
  CHECK(HorizontalDistance(a, kStart.position) ==
        doctest::Approx(ring.radius).epsilon(1e-3));

  // Different deaths visit different points of the ring.
  std::vector<glm::vec3> seen;
  for (uint32_t deaths = 0; deaths < 16; ++deaths) {
    const glm::vec3 p = ChooseSpawnPoint(world, kStart, 1, deaths, {}, 1.65f);
    bool known = false;
    for (const glm::vec3& q : seen) {
      known = known || glm::length(p - q) < 0.01f;
    }
    if (!known) {
      seen.push_back(p);
    }
  }
  CHECK(seen.size() > 2);

  // An occupied point is skipped.
  const std::vector<glm::vec3> occupied = {a};
  const glm::vec3 b = ChooseSpawnPoint(world, kStart, 1, 0, occupied, 1.65f);
  CHECK(HorizontalDistance(a, b) > ring.min_separation);
  CHECK(HorizontalDistance(b, kStart.position) ==
        doctest::Approx(ring.radius).epsilon(1e-3));
}

TEST_CASE("Blocked spawn points are skipped") {
  physics::PhysicsRuntime runtime;
  physics::PhysicsWorld world(runtime, {.worker_threads = 0});
  REQUIRE(world
              .CreateBody(StaticBox(glm::vec3(0.0f, -0.5f, 0.0f),
                                    glm::vec3(20.0f, 0.5f, 20.0f)))
              .ok());
  // A cell of four walls 1.5 m around the start: every ring point is behind
  // a wall, so the start's own floor is used.
  for (const glm::vec3 center :
       {glm::vec3(1.5f, 1.0f, 0.0f), glm::vec3(-1.5f, 1.0f, 0.0f)}) {
    REQUIRE(
        world.CreateBody(StaticBox(center, glm::vec3(0.1f, 2.0f, 2.0f))).ok());
  }
  for (const glm::vec3 center :
       {glm::vec3(0.0f, 1.0f, 1.5f), glm::vec3(0.0f, 1.0f, -1.5f)}) {
    REQUIRE(
        world.CreateBody(StaticBox(center, glm::vec3(2.0f, 2.0f, 0.1f))).ok());
  }
  const glm::vec3 p = ChooseSpawnPoint(world, kStart, 9, 3, {}, 1.65f);
  CHECK(p.x == doctest::Approx(0.0f));
  CHECK(p.z == doctest::Approx(0.0f));
  CHECK(p.y == doctest::Approx(0.0f).epsilon(1e-4));

  // Without any floor: the start lowered by the eye height.
  physics::PhysicsWorld empty(runtime, {.worker_threads = 0});
  CHECK(ChooseSpawnPoint(empty, kStart, 1, 0, {}, 1.65f) ==
        kStart.position - glm::vec3(0.0f, 1.65f, 0.0f));
}

TEST_CASE("A lethal hit drops the weapon; the victim respawns later") {
  const testing::TestScene files;
  physics::PhysicsRuntime runtime;
  auto created = SimWorld::Create(runtime, files.Options());
  REQUIRE(created.ok());
  SimWorld& world = *created.value();
  REQUIRE(world.AddPlayer(1));
  REQUIRE(world.AddPlayer(2));
  uint32_t tick = 1;
  for (; tick <= 10; ++tick) {
    testing::RunTick(world, tick);
  }
  (void)testing::DrainEvents(world);

  // One head shot: 137 HP.
  const glm::vec3 head = testing::CapsulePoint(world, 2, 0.95f);
  REQUIRE(world.ApplyCommand(
      1, testing::AimAt(world, 1, head, tick, buttons::held::kFire)));
  testing::RunTick(world, tick);
  const uint32_t death_tick = tick;
  std::vector<SimEvent> events = testing::DrainEvents(world);
  const WeaponKey rifle_key =
      KeyOfWeaponName(PrefabWeaponName("weapons/rifle.pbo"));
  bool damaged = false;
  bool died = false;
  std::optional<SimEvent> dropped;
  for (const SimEvent& event : events) {
    CHECK(event.tick == death_tick);
    // The damage event is the shooter's hit confirmation too: no other kind
    // repeats it.
    CHECK(static_cast<int>(event.type) <=
          static_cast<int>(SimEventType::kPickupTaken));
    if (event.type == SimEventType::kDamage) {
      damaged = true;
      CHECK(event.player == 2);
      CHECK(event.other == 1);
      CHECK(event.weapon_key == rifle_key);  // Player 1's rifle fired.
      CHECK(event.zone == HitZone::kHead);
      CHECK(event.value >= 130);
      CHECK(event.health == 0);
    } else if (event.type == SimEventType::kDeath) {
      died = true;
      CHECK(event.player == 2);
      CHECK(event.other == 1);
      CHECK(event.weapon_key == rifle_key);
    } else if (event.type == SimEventType::kWeaponDropped) {
      dropped = event;
      CHECK(event.player == 2);
      CHECK(event.weapon_key == rifle_key);
    }
  }
  CHECK(damaged);
  CHECK(died);
  REQUIRE(dropped.has_value());
  std::vector<DroppedReadout> items(4);
  REQUIRE(world.ReadDroppedWeapons(items) == 1);
  // The victim never fired: a full magazine and a chambered round, reported
  // alike by the event and the read-out, and the weapon flies off.
  const DroppedReadout& thrown = items[0];
  CHECK(thrown.id == dropped->id);
  CHECK(thrown.magazine > 0);
  CHECK(thrown.chambered);
  CHECK(dropped->value == thrown.magazine);
  CHECK(dropped->chambered == thrown.chambered);
  CHECK(thrown.active);
  CHECK(glm::length(thrown.linear_velocity) > 0.1f);
  const PlayerSim* victim = world.player(2);
  REQUIRE(victim != nullptr);
  CHECK_FALSE(victim->alive());
  const PlayerState dead = victim->Save();
  CHECK(dead.respawn_ticks == kRespawnTicks);
  CHECK(dead.deaths == 1);
  CHECK_FALSE(dead.loadout.slots[1].present);  // The rifle is on the floor.

  // Dead players are not hit.
  const glm::vec3 body = testing::CapsulePoint(world, 2, 0.5f);
  ++tick;
  for (int i = 0; i < kRespawnTicks - 1; ++i, ++tick) {
    REQUIRE(world.ApplyCommand(
        1, testing::AimAt(world, 1, body, tick,
                          i % 2 == 0 ? buttons::held::kFire : 0u)));
    testing::RunTick(world, tick);
    for (const SimEvent& event : testing::DrainEvents(world)) {
      CHECK(event.type != SimEventType::kDamage);
      CHECK(event.type != SimEventType::kRespawn);
    }
  }
  CHECK_FALSE(victim->alive());
  // The kRespawnTicks-th tick after the death brings it back.
  testing::RunTick(world, tick);
  CHECK(tick == death_tick + kRespawnTicks);
  bool respawned = false;
  for (const SimEvent& event : testing::DrainEvents(world)) {
    if (event.type == SimEventType::kRespawn) {
      respawned = true;
      CHECK(event.player == 2);
      CHECK(event.yaw == world.scene().player_start.yaw_radians);
    }
  }
  CHECK(respawned);
  REQUIRE(victim->alive());
  const PlayerState back = victim->Save();
  CHECK(back.health == kMaxHealth);
  CHECK(back.deaths == 1);
  CHECK(back.loadout.slots[0].present);
  CHECK(back.loadout.slots[1].present);
  CHECK(back.position.y == doctest::Approx(0.0f).epsilon(0.05));
  CHECK(
      HorizontalDistance(back.position, world.scene().player_start.position) ==
      doctest::Approx(SpawnRing{}.radius).epsilon(0.05));
  CHECK(HorizontalDistance(back.position, world.player(1)->feet()) >
        SpawnRing{}.min_separation);
}

}  // namespace
}  // namespace blue::sim
