#include "sim/player_sim.h"

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
#include "scene/scene.h"
#include "sim/player_state.h"
#include "sim/user_command.h"
#include "sim/weapon_table.h"
#include "tests/sim/sim_test_scene.h"
#include "weapon/firearm.h"
#include "weapon/loadout.h"
#include "weapon/weapon_def.h"
#include "weapon/weapon_item.h"

namespace blue::sim {
namespace {

namespace held = buttons::held;
namespace pressed = buttons::pressed;

// A floor, a weapon library with a pistol, a rifle and a copy of the rifle
// that spreads its shots (30 MOA), and their table.
struct Fixture {
  physics::PhysicsRuntime runtime;
  physics::PhysicsWorld world{runtime, {.worker_threads = 0}};
  weapon::WeaponLibrary library;
  weapon::WeaponDefId pistol{};
  weapon::WeaponDefId rifle{};
  weapon::WeaponDefId spread{};
  WeaponTable table;

  Fixture() {
    physics::BodyDesc floor;
    floor.body.motion = scene::MotionType::kStatic;
    floor.body.shape = scene::ShapeType::kBox;
    floor.pose.position = glm::vec3(0.0f, -0.5f, 0.0f);
    floor.bounds = {.min = glm::vec3(-50.0f, -0.5f, -50.0f),
                    .max = glm::vec3(50.0f, 0.5f, 50.0f)};
    REQUIRE(world.CreateBody(floor).ok());

    pistol =
        *library.Register(1, testing::MakeWeaponPrefab("glock17"), "pistol");
    rifle = *library.Register(2, testing::MakeWeaponPrefab("m16"), "rifle");
    weapon::WeaponDef inaccurate = library.def(rifle);
    inaccurate.accuracy_moa = 30.0f;
    spread = library.AddBuild(std::move(inaccurate));
    const std::vector<WeaponTableEntry> entries = {
        {.name = "prefab:pistol.pbo", .def = pistol},
        {.name = "prefab:rifle.pbo", .def = rifle},
        {.name = "preset:spread", .def = spread}};
    auto built = WeaponTable::Build(entries, library);
    REQUIRE(built.ok());
    table = std::move(built).value();
  }

  weapon::Loadout MakeLoadout(weapon::WeaponDefId long_gun) const {
    const std::vector<weapon::WeaponItem> weapons = {
        weapon::MakeFreshWeapon(pistol, library.def(pistol)),
        weapon::MakeFreshWeapon(long_gun, library.def(long_gun))};
    return weapon::Loadout(weapons);
  }

  std::unique_ptr<PlayerSim> MakePlayer(uint32_t id, glm::vec3 feet,
                                        weapon::WeaponDefId long_gun) {
    auto created =
        PlayerSim::Create(world, library, table, id, feet, camera::LookAngles{},
                          MakeLoadout(long_gun));
    REQUIRE(created.ok());
    return std::move(created).value();
  }
};

// Exact comparison of everything a state holds, float bits included.
void CheckSameState(const PlayerState& a, const PlayerState& b) {
  CHECK(a.position == b.position);
  CHECK(a.velocity == b.velocity);
  CHECK(a.jump_speed == b.jump_speed);
  CHECK(a.jump_age == b.jump_age);
  CHECK(a.jump_cooldown == b.jump_cooldown);
  CHECK(a.jump_pending == b.jump_pending);
  CHECK(a.crouching == b.crouching);
  CHECK(a.on_ground == b.on_ground);
  CHECK(a.yaw == b.yaw);
  CHECK(a.pitch == b.pitch);
  CHECK(a.eye_height == b.eye_height);
  CHECK(a.sprinting == b.sprinting);
  CHECK(a.aiming == b.aiming);
  CHECK(a.alive == b.alive);
  CHECK(a.health == b.health);
  CHECK(a.respawn_ticks == b.respawn_ticks);
  CHECK(a.shots_fired == b.shots_fired);
  CHECK(a.deaths == b.deaths);
  CHECK(a.loadout == b.loadout);
  CHECK(a.slot_keys == b.slot_keys);
}

// The scripted input of tick `tick`: walking, a jump, crouching, sprinting,
// automatic fire, a reload and weapon switches, turning all the while.
UserCommand ScriptedCommand(uint32_t tick) {
  UserCommand command;
  command.tick = tick;
  command.yaw = 0.004f * static_cast<float>(tick);
  command.pitch = 0.2f * std::sin(0.05f * static_cast<float>(tick));
  if (tick < 60) {
    command.held |= held::kForward;
  }
  if (tick == 20 || tick == 170) {
    command.pressed |= pressed::kJump;
  }
  if (tick >= 100 && tick < 140) {
    command.held |= held::kCrouch | held::kLeft;
  }
  if (tick >= 150 && tick < 200) {
    command.held |= held::kSprint | held::kForward;
  }
  if (tick == 25) {
    command.pressed |= pressed::kFireMode;  // To automatic.
  }
  if ((tick >= 30 && tick < 80) || (tick >= 270 && tick < 285)) {
    command.held |= held::kFire;
  }
  if (tick == 85) {
    command.pressed |= pressed::kReload;
  }
  if (tick == 230) {
    command.select_slot = 0;  // The pistol.
  }
  if (tick == 255) {
    command.select_slot = 1;  // The rifle again.
  }
  if (tick >= 240 && tick < 250 && tick % 3 == 0) {
    command.held |= held::kFire;  // Pistol shots once it is up.
  }
  return command;
}

TEST_CASE("Restoring a state and replaying equals the straight run") {
  Fixture fixture;
  std::unique_ptr<PlayerSim> player =
      fixture.MakePlayer(7, glm::vec3(0.0f), fixture.rifle);
  for (int i = 0; i < 5; ++i) {
    (void)player->Step(UserCommand{});  // Settle on the floor.
  }

  constexpr uint32_t kTicks = 300;
  std::vector<PlayerState> states;
  std::vector<StepEvents> events;
  states.push_back(player->Save());
  for (uint32_t tick = 1; tick <= kTicks; ++tick) {
    events.push_back(player->Step(ScriptedCommand(tick)));
    states.push_back(player->Save());
  }
  // The script did what it says.
  int shots = 0;
  bool jumped = false;
  bool crouched = false;
  bool sprinted = false;
  bool reloaded = false;
  bool switched = false;
  for (size_t i = 0; i < events.size(); ++i) {
    shots += events[i].shot_count;
    reloaded = reloaded || events[i].reload_started;
    switched = switched || events[i].weapon_changed;
    jumped = jumped || states[i + 1].position.y > 0.3f;
    crouched = crouched || states[i + 1].crouching;
    sprinted = sprinted || states[i + 1].sprinting;
  }
  CHECK(shots > 10);
  CHECK(jumped);
  CHECK(crouched);
  CHECK(sprinted);
  CHECK(reloaded);
  CHECK(switched);
  CHECK(states.back().shots_fired == static_cast<uint32_t>(shots));

  for (const uint32_t from : {0u, 1u, 45u, 101u, 175u, 240u, 299u}) {
    CAPTURE(from);
    REQUIRE(player->Restore(states[from]));
    CheckSameState(player->Save(), states[from]);
    for (uint32_t tick = from + 1; tick <= kTicks; ++tick) {
      CAPTURE(tick);
      const StepEvents replayed = player->Step(ScriptedCommand(tick));
      const StepEvents& original = events[tick - 1];
      REQUIRE(replayed.shot_count == original.shot_count);
      for (int s = 0; s < replayed.shot_count; ++s) {
        const auto index = static_cast<size_t>(s);
        CHECK(replayed.shots[index].direction ==
              original.shots[index].direction);
        CHECK(replayed.shots[index].origin == original.shots[index].origin);
        CHECK(replayed.shots[index].shot_number ==
              original.shots[index].shot_number);
      }
      const PlayerState state = player->Save();
      CheckSameState(state, states[tick]);
      CHECK_FALSE(NeedsReplay(state, states[tick]));
    }
  }
}

TEST_CASE("Shot dispersion follows the shot sequence, not the tick") {
  CHECK(ShotSeed(1, 1) == ShotSeed(1, 1));
  CHECK(ShotSeed(1, 1) != ShotSeed(1, 2));
  CHECK(ShotSeed(1, 1) != ShotSeed(2, 1));
  for (uint32_t i = 0; i < 1000; ++i) {
    CHECK(ShotSeed(i, i * 7) != 0);
  }

  Fixture fixture;
  std::unique_ptr<PlayerSim> player =
      fixture.MakePlayer(3, glm::vec3(0.0f), fixture.spread);
  (void)player->Step(UserCommand{});
  const PlayerState before = player->Save();
  UserCommand fire;
  fire.held = held::kFire;
  fire.tick = 100;
  const StepEvents first = player->Step(fire);
  REQUIRE(first.shot_count == 1);
  CHECK(first.shots[0].shot_number == before.shots_fired + 1);
  CHECK(first.shots[0].weapon_key == KeyOfWeaponName("preset:spread"));
  // Spread off the zeroed look direction.
  CHECK(glm::length(first.shots[0].direction) ==
        doctest::Approx(1.0f).epsilon(1e-5));
  const glm::vec3 look = camera::ForwardFrom(player->look());
  CHECK(glm::dot(first.shots[0].direction, look) < 0.999999f);
  CHECK(first.shots[0].origin == player->eye());

  // The same shot predicted a tick late spreads the same.
  REQUIRE(player->Restore(before));
  fire.tick = 101;
  const StepEvents late = player->Step(fire);
  REQUIRE(late.shot_count == 1);
  CHECK(late.shots[0].direction == first.shots[0].direction);

  // Another player's shot of the same number spreads differently.
  std::unique_ptr<PlayerSim> other =
      fixture.MakePlayer(4, glm::vec3(0.0f), fixture.spread);
  (void)other->Step(UserCommand{});
  REQUIRE(other->Restore(before));
  const StepEvents others = other->Step(fire);
  REQUIRE(others.shot_count == 1);
  CHECK(others.shots[0].direction != first.shots[0].direction);
}

TEST_CASE("Dead players ignore commands and do not move") {
  Fixture fixture;
  std::unique_ptr<PlayerSim> player =
      fixture.MakePlayer(1, glm::vec3(0.0f), fixture.rifle);
  for (int i = 0; i < 5; ++i) {
    (void)player->Step(UserCommand{});
  }
  player->Kill(3);
  CHECK_FALSE(player->alive());
  const PlayerState dead = player->Save();
  CHECK(dead.health == 0);
  CHECK(dead.deaths == 1);
  CHECK(dead.respawn_ticks == 3);
  UserCommand command;
  command.held = held::kForward | held::kFire;
  command.pressed = pressed::kJump;
  for (int i = 0; i < 30; ++i) {
    const StepEvents events = player->Step(command);
    CHECK(events.shot_count == 0);
  }
  CHECK(player->feet() == dead.position);
  CHECK(player->Save().shots_fired == dead.shots_fired);
  player->Kill(3);  // Already dead: nothing changes.
  CHECK(player->Save().deaths == 1);

  CHECK_FALSE(player->CountDownRespawn());
  CHECK_FALSE(player->CountDownRespawn());
  CHECK(player->CountDownRespawn());
  player->Respawn(glm::vec3(2.0f, 0.0f, 0.0f), camera::LookAngles{},
                  fixture.MakeLoadout(fixture.rifle));
  CHECK(player->alive());
  CHECK(player->health() == kMaxHealth);
  (void)player->Step(command);
  CHECK(player->feet() != glm::vec3(2.0f, 0.0f, 0.0f));
}

TEST_CASE("Restore swaps in the weapons a state names") {
  Fixture fixture;
  std::unique_ptr<PlayerSim> spread =
      fixture.MakePlayer(1, glm::vec3(0.0f), fixture.spread);
  std::unique_ptr<PlayerSim> player =
      fixture.MakePlayer(2, glm::vec3(3.0f, 0.0f, 0.0f), fixture.rifle);
  const PlayerState other = spread->Save();
  CHECK(other.slot_keys[1] == KeyOfWeaponName("preset:spread"));
  REQUIRE(player->Restore(other));
  CheckSameState(player->Save(), other);

  PlayerState unknown = other;
  unknown.slot_keys[1] = 12345;
  CHECK_FALSE(player->Restore(unknown));
}

TEST_CASE("Replay thresholds") {
  PlayerState a;
  a.position = glm::vec3(1.0f, 2.0f, 3.0f);
  PlayerState b = a;
  CHECK_FALSE(NeedsReplay(a, b));
  b.position.x += 0.009f;
  CHECK_FALSE(NeedsReplay(a, b));
  b.position.x += 0.002f;
  CHECK(NeedsReplay(a, b));
  b = a;
  b.velocity.y = 0.09f;
  CHECK_FALSE(NeedsReplay(a, b));
  b.velocity.y = 0.11f;
  CHECK(NeedsReplay(a, b));
  b = a;
  b.yaw = 1.0f;
  b.pitch = -0.5f;
  CHECK_FALSE(NeedsReplay(a, b));  // Angles come from the command.
  b = a;
  b.crouching = true;
  CHECK(NeedsReplay(a, b));
  b = a;
  b.jump_age = 1;
  CHECK(NeedsReplay(a, b));
  b = a;
  b.health = 99;
  CHECK(NeedsReplay(a, b));
  b = a;
  b.loadout.slots[0].present = true;
  b.loadout.slots[0].firearm.cycle_remaining = 0.0005f;
  PlayerState c = b;
  c.loadout.slots[0].firearm.cycle_remaining = 0.0f;
  CHECK_FALSE(NeedsReplay(b, c));  // Within 1 ms.
  c.loadout.slots[0].firearm.cycle_remaining = 0.002f;
  CHECK(NeedsReplay(b, c));
  c = b;
  c.slot_keys[0] = 5;
  CHECK(NeedsReplay(b, c));
}

}  // namespace
}  // namespace blue::sim
