#include "sim/sim_world.h"

#include <doctest/doctest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

#include "camera/look.h"
#include "physics/physics_runtime.h"
#include "physics/physics_world.h"
#include "sim/damage.h"
#include "sim/player_state.h"
#include "sim/user_command.h"
#include "sim/weapon_table.h"
#include "tests/sim/sim_test_scene.h"

namespace blue::sim {
namespace {

namespace held = buttons::held;
namespace pressed = buttons::pressed;

const WeaponKey kRifleKey =
    KeyOfWeaponName(PrefabWeaponName("weapons/rifle.pbo"));

// A SimWorld on the test scene with its own physics runtime.
struct Fixture {
  testing::TestScene files;
  physics::PhysicsRuntime runtime;
  std::unique_ptr<SimWorld> world;
  uint32_t tick = 0;

  Fixture() : Fixture(SimWorldOptions{}.dropped_lifetime_ticks) {}
  // A world with the given dropped-weapon lifetime and cap and kill-plane
  // depth.
  explicit Fixture(uint32_t dropped_lifetime_ticks, int max_dropped = 64,
                   float kill_plane_depth = 50.0f) {
    SimWorldOptions options = files.Options();
    options.dropped_lifetime_ticks = dropped_lifetime_ticks;
    options.max_dropped_weapons = max_dropped;
    options.kill_plane_depth = kill_plane_depth;
    auto created = SimWorld::Create(runtime, options);
    REQUIRE(created.ok());
    world = std::move(created).value();
  }

  // Player `id` throws the weapon in hand down at its feet.
  void Drop(uint32_t id) {
    UserCommand drop;
    drop.tick = ++tick;
    drop.pitch = -1.5f;
    drop.pressed = pressed::kDropWeapon;
    REQUIRE(world->ApplyCommand(id, drop));
    testing::RunTick(*world, tick);
  }

  // Runs `count` ticks with no commands.
  void Idle(int count) {
    for (int i = 0; i < count; ++i) {
      testing::RunTick(*world, ++tick);
    }
  }

  // Walks player `id` to within `distance` m (horizontally) of `target`.
  void WalkTo(uint32_t id, glm::vec3 target, float distance) {
    for (int i = 0; i < 600; ++i) {
      const glm::vec3 feet = world->player(id)->feet();
      const glm::vec3 flat(target.x - feet.x, 0.0f, target.z - feet.z);
      if (glm::length(flat) < distance) {
        Idle(10);  // Come to rest.
        return;
      }
      const camera::LookAngles look = camera::LookTowards(feet, feet + flat);
      UserCommand command;
      command.tick = ++tick;
      command.held = held::kForward;
      command.yaw = look.yaw;
      REQUIRE(world->ApplyCommand(id, command));
      testing::RunTick(*world, tick);
    }
    FAIL("player did not get there");
  }

  PlayerState State(uint32_t id) const { return world->player(id)->Save(); }
};

int Count(const std::vector<SimEvent>& events, SimEventType type) {
  int count = 0;
  for (const SimEvent& event : events) {
    count += event.type == type ? 1 : 0;
  }
  return count;
}

TEST_CASE("SimWorld fails cleanly on bad input") {
  const testing::TestScene files;
  physics::PhysicsRuntime runtime;
  SimWorldOptions options = files.Options();
  options.scene_path = "missing.mscn";
  auto missing = SimWorld::Create(runtime, options);
  REQUIRE_FALSE(missing.ok());
  CHECK(missing.error().find("missing.mscn") != std::string::npos);

  options = files.Options();
  options.starting_weapons = {{.path = "weapons/none.pbo"}};
  auto no_weapon = SimWorld::Create(runtime, options);
  REQUIRE_FALSE(no_weapon.ok());
  CHECK(no_weapon.error().find("weapons/none.pbo") != std::string::npos);

  options = files.Options();
  options.asset_dir.clear();
  CHECK_FALSE(SimWorld::Create(runtime, options).ok());
}

TEST_CASE("SimWorld builds the scene, its weapons and pickups") {
  Fixture fixture;
  SimWorld& world = *fixture.world;
  CHECK(world.weapon_table().size() == 2);
  CHECK(world.content_hash() != 0);
  CHECK(world.content_hash() != world.weapon_table().content_hash());

  std::vector<BodyReadout> bodies(8);
  REQUIRE(world.ReadBodies(bodies) == 1);
  CHECK(bodies[0].instance == testing::kCrateInstance);
  std::vector<PickupReadout> pickups(8);
  REQUIRE(world.ReadPickups(pickups) == 1);
  CHECK(pickups[0].instance == testing::kPickupInstance);
  CHECK(pickups[0].weapon_key == kRifleKey);
  CHECK_FALSE(pickups[0].taken);
  CHECK(glm::length(pickups[0].pose.position - testing::kPickupPosition) <
        1e-5f);

  // Players, by id, up to max_players (8).
  for (uint32_t id = 10; id > 2; --id) {
    CHECK(world.AddPlayer(id));
  }
  CHECK_FALSE(world.AddPlayer(10));  // In use.
  CHECK_FALSE(world.AddPlayer(1));   // Full.
  std::vector<PlayerReadout> players(3);
  CHECK(world.ReadPlayers(players) == 8);
  CHECK(players[0].id == 3);
  CHECK(players[1].id == 4);
  CHECK(players[2].id == 5);
  CHECK(players[0].state.health == kMaxHealth);
  CHECK(players[0].state.slot_keys[1] == kRifleKey);
  CHECK_FALSE(world.ApplyCommand(99, UserCommand{}));
  CHECK_FALSE(world.CurrentCapsule(99).has_value());

  // A ray from above finds the crate.
  const std::optional<SimRayHit> hit =
      world.CastRay(testing::kCratePosition + glm::vec3(0.0f, 5.0f, 0.0f),
                    glm::vec3(0.0f, -1.0f, 0.0f), 10.0f);
  REQUIRE(hit.has_value());
  CHECK(hit->instance == testing::kCrateInstance);
  CHECK(hit->point.y == doctest::Approx(1.0f).epsilon(1e-3));
}

TEST_CASE("One player shoots another until it dies") {
  Fixture fixture;
  SimWorld& world = *fixture.world;
  REQUIRE(world.AddPlayer(1));
  REQUIRE(world.AddPlayer(2));
  fixture.Idle(10);
  (void)testing::DrainEvents(world);

  std::vector<SimEvent> events;
  for (int i = 0; i < 120 && world.player(2)->alive(); ++i) {
    const uint32_t tick = ++fixture.tick;
    // Torso shots, releasing the (semi-automatic) trigger in between.
    const glm::vec3 chest = testing::CapsulePoint(world, 2, 0.6f);
    REQUIRE(world.ApplyCommand(
        1,
        testing::AimAt(world, 1, chest, tick, i % 10 == 0 ? held::kFire : 0u)));
    testing::RunTick(world, tick);
    for (const SimEvent& event : testing::DrainEvents(world)) {
      events.push_back(event);
    }
  }
  CHECK_FALSE(world.player(2)->alive());
  CHECK(Count(events, SimEventType::kShot) == 3);
  CHECK(Count(events, SimEventType::kDamage) == 3);
  CHECK(Count(events, SimEventType::kDeath) == 1);
  int health = kMaxHealth;
  for (const SimEvent& event : events) {
    if (event.type == SimEventType::kDamage) {
      CHECK(event.other == 1);
      CHECK(event.weapon_key == kRifleKey);
      CHECK(event.zone == HitZone::kTorso);
      CHECK(event.value == doctest::Approx(34).epsilon(0.05));
      health -= event.value;
      CHECK(event.health == std::max(health, 0));
    }
    if (event.type == SimEventType::kShot) {
      CHECK(event.player == 1);
      CHECK(event.weapon_key == kRifleKey);
    }
    if (event.type == SimEventType::kDeath) {
      CHECK(event.player == 2);
      CHECK(event.other == 1);
      CHECK(event.weapon_key == kRifleKey);
    }
  }
  CHECK(fixture.State(1).shots_fired == 3);
}

TEST_CASE("A bullet pushes the crate") {
  Fixture fixture;
  SimWorld& world = *fixture.world;
  REQUIRE(world.AddPlayer(1));
  fixture.Idle(30);  // The crate settles.
  std::vector<BodyReadout> before(1);
  REQUIRE(world.ReadBodies(before) == 1);
  (void)testing::DrainEvents(world);

  const uint32_t tick = ++fixture.tick;
  REQUIRE(world.ApplyCommand(
      1, testing::AimAt(world, 1, testing::kCratePosition, tick, held::kFire)));
  testing::RunTick(world, tick);
  const std::vector<SimEvent> events = testing::DrainEvents(world);
  bool impact = false;
  for (const SimEvent& event : events) {
    if (event.type == SimEventType::kImpact) {
      impact = true;
      CHECK(event.id == testing::kCrateInstance);
    }
  }
  CHECK(impact);
  fixture.Idle(1);
  std::vector<BodyReadout> after(1);
  REQUIRE(world.ReadBodies(after) == 1);
  CHECK(after[0].state.active);
  CHECK(glm::length(after[0].state.linear_velocity) > 0.3f);
}

TEST_CASE("Weapons are dropped, picked up and taken from the scene") {
  Fixture fixture;
  SimWorld& world = *fixture.world;
  REQUIRE(world.AddPlayer(1));
  fixture.Idle(5);
  (void)testing::DrainEvents(world);

  // Drop the rifle at the feet (looking down) and let it land.
  UserCommand drop;
  drop.tick = ++fixture.tick;
  drop.pitch = -1.5f;
  drop.pressed = pressed::kDropWeapon;
  REQUIRE(world.ApplyCommand(1, drop));
  testing::RunTick(world, fixture.tick);
  std::vector<SimEvent> events = testing::DrainEvents(world);
  REQUIRE(Count(events, SimEventType::kWeaponDropped) == 1);
  CHECK_FALSE(fixture.State(1).loadout.slots[1].present);
  fixture.Idle(90);
  std::vector<DroppedReadout> items(2);
  REQUIRE(world.ReadDroppedWeapons(items) == 1);
  CHECK(items[0].weapon_key == kRifleKey);
  const uint32_t dropped_id = items[0].id;

  // Out of reach or not aimed at: refused.
  UserCommand wrong = testing::AimAt(
      world, 1, items[0].pose.position + glm::vec3(0.0f, 5.0f, 0.0f),
      ++fixture.tick, 0, pressed::kInteract);
  wrong.interact_kind = interact_kind::kDroppedWeapon;
  wrong.interact_id = dropped_id;
  REQUIRE(world.ApplyCommand(1, wrong));
  testing::RunTick(world, fixture.tick);
  CHECK(Count(testing::DrainEvents(world), SimEventType::kDroppedTaken) == 0);

  // Picked up again.
  UserCommand take = testing::AimAt(world, 1, items[0].pose.position,
                                    ++fixture.tick, 0, pressed::kInteract);
  take.interact_kind = interact_kind::kDroppedWeapon;
  take.interact_id = dropped_id;
  REQUIRE(world.ApplyCommand(1, take));
  testing::RunTick(world, fixture.tick);
  events = testing::DrainEvents(world);
  CHECK(Count(events, SimEventType::kDroppedTaken) == 1);
  CHECK(world.ReadDroppedWeapons(items) == 0);
  CHECK(fixture.State(1).loadout.slots[1].present);
  CHECK(fixture.State(1).slot_keys[1] == kRifleKey);

  // The scene's rifle: taking it swaps out the one carried.
  fixture.WalkTo(1, testing::kPickupPosition, 0.8f);
  UserCommand pick = testing::AimAt(world, 1, testing::kPickupPosition,
                                    ++fixture.tick, 0, pressed::kInteract);
  pick.interact_kind = interact_kind::kSceneWeapon;
  pick.interact_id = testing::kPickupInstance;
  REQUIRE(world.ApplyCommand(1, pick));
  testing::RunTick(world, fixture.tick);
  events = testing::DrainEvents(world);
  CHECK(Count(events, SimEventType::kPickupTaken) == 1);
  CHECK(Count(events, SimEventType::kWeaponDropped) == 1);
  std::vector<PickupReadout> pickups(1);
  REQUIRE(world.ReadPickups(pickups) == 1);
  CHECK(pickups[0].taken);
  CHECK(world.ReadDroppedWeapons(items) == 1);

  // A taken pickup cannot be taken again.
  pick.tick = ++fixture.tick;
  REQUIRE(world.ApplyCommand(1, pick));
  testing::RunTick(world, fixture.tick);
  CHECK(Count(testing::DrainEvents(world), SimEventType::kPickupTaken) == 0);
}

TEST_CASE("Dropped weapons despawn when their lifetime runs out") {
  Fixture fixture(30);
  SimWorld& world = *fixture.world;
  REQUIRE(world.AddPlayer(1));
  fixture.Idle(5);
  (void)testing::DrainEvents(world);
  fixture.Drop(1);
  std::vector<SimEvent> events = testing::DrainEvents(world);
  REQUIRE(Count(events, SimEventType::kWeaponDropped) == 1);
  const uint32_t id = events.back().id;
  std::vector<DroppedReadout> items(4);
  fixture.Idle(29);
  CHECK(world.ReadDroppedWeapons(items) == 1);
  CHECK(Count(testing::DrainEvents(world), SimEventType::kDroppedRemoved) == 0);
  fixture.Idle(1);  // 30 ticks after the drop.
  CHECK(world.ReadDroppedWeapons(items) == 0);
  events = testing::DrainEvents(world);
  REQUIRE(Count(events, SimEventType::kDroppedRemoved) == 1);
  CHECK(events.back().id == id);
  CHECK(events.back().weapon_key == kRifleKey);
}

TEST_CASE("The dropped-weapon cap removes the oldest one first") {
  Fixture fixture(7200, 1);
  SimWorld& world = *fixture.world;
  REQUIRE(world.AddPlayer(1));
  fixture.Idle(5);
  fixture.Drop(1);  // The rifle.
  std::vector<SimEvent> events = testing::DrainEvents(world);
  REQUIRE(Count(events, SimEventType::kWeaponDropped) == 1);
  const uint32_t rifle_id = events.back().id;
  // Take the pistol out, then drop it too.
  UserCommand select;
  select.tick = ++fixture.tick;
  select.select_slot = 0;
  REQUIRE(world.ApplyCommand(1, select));
  testing::RunTick(world, fixture.tick);
  fixture.Idle(90);
  (void)testing::DrainEvents(world);
  fixture.Drop(1);
  events = testing::DrainEvents(world);
  REQUIRE(Count(events, SimEventType::kDroppedRemoved) == 1);
  REQUIRE(Count(events, SimEventType::kWeaponDropped) == 1);
  // The removal comes first and names the rifle.
  CHECK(events[0].type == SimEventType::kDroppedRemoved);
  CHECK(events[0].id == rifle_id);
  std::vector<DroppedReadout> items(4);
  REQUIRE(world.ReadDroppedWeapons(items) == 1);
  CHECK(items[0].weapon_key != kRifleKey);
}

TEST_CASE("Falling below the kill plane kills; weapons there are removed") {
  Fixture fixture(7200, 64, 1.0f);
  SimWorld& world = *fixture.world;
  // The floor's underside is y = -1.
  CHECK(world.kill_plane_y() == doctest::Approx(-2.0f));
  REQUIRE(world.AddPlayer(1));
  fixture.Idle(5);
  (void)testing::DrainEvents(world);
  // Walk off the floor's +X edge (x = 20), throw the weapon on the way down.
  bool dropped = false;
  bool died = false;
  bool removed = false;
  for (int i = 0; i < 900 && !(died && removed); ++i) {
    UserCommand command;
    command.tick = ++fixture.tick;
    command.held = held::kForward | held::kSprint;
    command.yaw =
        camera::LookTowards(glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f)).yaw;
    if (!dropped && world.player(1)->feet().y < -0.5f) {
      command.pressed = pressed::kDropWeapon;
      dropped = true;
    }
    REQUIRE(world.ApplyCommand(1, command));
    testing::RunTick(world, fixture.tick);
    for (const SimEvent& event : testing::DrainEvents(world)) {
      if (event.type == SimEventType::kDeath) {
        CHECK(event.player == 1);
        CHECK(event.other == 0);
        CHECK(event.weapon_key == 0);
        CHECK(event.position.y < world.kill_plane_y());
        died = true;
      }
      removed = removed || event.type == SimEventType::kDroppedRemoved;
      // A kill-plane death drops nothing.
      if (died && !removed) {
        CHECK(event.type != SimEventType::kWeaponDropped);
      }
    }
  }
  CHECK(dropped);
  CHECK(died);
  CHECK(removed);
  CHECK_FALSE(world.player(1)->alive());
  std::vector<DroppedReadout> items(4);
  CHECK(world.ReadDroppedWeapons(items) == 0);
  // The normal respawn follows, on the floor.
  fixture.Idle(kRespawnTicks + 5);
  CHECK(world.player(1)->alive());
  CHECK(world.player(1)->feet().y > world.kill_plane_y());
}

TEST_CASE("A missing command repeats the held buttons without edges") {
  Fixture fixture;
  SimWorld& world = *fixture.world;
  REQUIRE(world.AddPlayer(1));
  fixture.Idle(5);
  const glm::vec3 start = world.player(1)->feet();

  UserCommand command;
  command.tick = ++fixture.tick;
  command.held = held::kForward;
  command.pressed = pressed::kJump;
  command.yaw = 0.5f;
  REQUIRE(world.ApplyCommand(1, command));
  testing::RunTick(world, fixture.tick);
  // Ninety ticks without commands: still walking along yaw 0.5, one jump
  // (about a second in the air).
  float highest = 0.0f;
  int landings = 0;
  bool airborne = false;
  for (int i = 0; i < 90; ++i) {
    testing::RunTick(world, ++fixture.tick);
    const PlayerState state = fixture.State(1);
    highest = std::max(highest, state.position.y);
    if (airborne && state.on_ground) {
      ++landings;
    }
    airborne = !state.on_ground;
    CHECK(state.yaw == 0.5f);
  }
  const glm::vec3 moved = world.player(1)->feet() - start;
  const glm::vec3 forward = camera::FlatForwardFrom(0.5f);
  CHECK(glm::dot(moved, forward) > 5.0f);
  CHECK(highest > 1.0f);
  CHECK(landings == 1);
  // Nor do the ticks after that.
  for (int i = 0; i < 40; ++i) {
    testing::RunTick(world, ++fixture.tick);
    CHECK(fixture.State(1).position.y < 0.05f);
  }
}

TEST_CASE("A player removed mid-game is gone") {
  Fixture fixture;
  SimWorld& world = *fixture.world;
  REQUIRE(world.AddPlayer(1));
  REQUIRE(world.AddPlayer(2));
  fixture.Idle(5);
  // Player 2 fires and leaves while its bullet flies.
  const uint32_t tick = ++fixture.tick;
  REQUIRE(world.ApplyCommand(
      2, testing::AimAt(world, 2, testing::kCratePosition, tick, held::kFire)));
  world.Step(tick);
  CHECK_FALSE(world.SweepProjectiles().empty());
  CHECK(world.RemovePlayer(2));
  CHECK_FALSE(world.RemovePlayer(2));
  std::vector<ShotHit> hits;
  world.HitCapsulesNow(hits);
  world.ResolveProjectiles(hits);
  CHECK_FALSE(world.ApplyCommand(2, UserCommand{}));
  CHECK_FALSE(world.CurrentCapsule(2).has_value());
  CHECK(world.player(2) == nullptr);
  fixture.Idle(20);
  std::vector<PlayerReadout> players(4);
  REQUIRE(world.ReadPlayers(players) == 1);
  CHECK(players[0].id == 1);
  CHECK(world.AddPlayer(2));  // The id may come back.
}

TEST_CASE("Segments hit capsules where they enter") {
  const Capsule capsule{
      .feet = glm::vec3(0.0f), .height = 1.8f, .radius = 0.35f};
  // Straight through the torso along -Z from 10 m.
  std::optional<float> t = SegmentHitsCapsule(
      glm::vec3(0.0f, 1.0f, 10.0f), glm::vec3(0.0f, 0.0f, -20.0f), capsule);
  REQUIRE(t.has_value());
  CHECK(*t == doctest::Approx((10.0f - 0.35f) / 20.0f));
  // Down onto the head.
  t = SegmentHitsCapsule(glm::vec3(0.0f, 5.0f, 0.0f),
                         glm::vec3(0.0f, -5.0f, 0.0f), capsule);
  REQUIRE(t.has_value());
  CHECK(*t == doctest::Approx((5.0f - 1.8f) / 5.0f));
  // Missing beside it, stopping short, starting inside.
  CHECK_FALSE(SegmentHitsCapsule(glm::vec3(0.5f, 1.0f, 10.0f),
                                 glm::vec3(0.0f, 0.0f, -20.0f), capsule));
  CHECK_FALSE(SegmentHitsCapsule(glm::vec3(0.0f, 1.0f, 10.0f),
                                 glm::vec3(0.0f, 0.0f, -5.0f), capsule));
  CHECK(SegmentHitsCapsule(glm::vec3(0.0f, 1.0f, 0.0f),
                           glm::vec3(0.0f, 0.0f, -5.0f), capsule) == 0.0f);
}

}  // namespace
}  // namespace blue::sim
