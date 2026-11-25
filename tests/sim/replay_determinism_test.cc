// The cross-platform determinism gate: a fixed 600-tick game of two players
// on the test scene, every tick's state hashed into one 64-bit value that
// must equal the constant below on MSVC (Windows) and GCC (Linux) alike. The
// dedicated server (Linux) and the client's prediction (Windows) run the same
// simulation; if they stop agreeing bit for bit, this test says so first.
//
// On a mismatch the running hash after every tick is printed: compare the
// output of two platforms to find the first tick where they diverge.

#include <doctest/doctest.h>

#include <bit>
#include <cstdint>
#include <cstdio>
#include <format>
#include <memory>
#include <vector>

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include "physics/physics_runtime.h"
#include "physics/physics_world.h"
#include "sim/player_state.h"
#include "sim/sim_world.h"
#include "sim/user_command.h"
#include "tests/sim/sim_test_scene.h"
#include "weapon/firearm.h"
#include "weapon/loadout.h"

namespace blue::sim {
namespace {

namespace held = buttons::held;
namespace pressed = buttons::pressed;

// The hash of the whole run. Changing the simulation on purpose changes it:
// update it from the failure message on one platform, then confirm the other
// platform agrees.
constexpr uint64_t kExpectedHash = 0xc4ee112372c11ce1ull;

constexpr uint32_t kTicks = 600;

// 64-bit FNV-1a over values added one by one, floats by their bits.
class Hasher {
 public:
  void Add(uint64_t value) {
    for (int i = 0; i < 8; ++i) {
      hash_ ^= (value >> (8 * i)) & 0xffu;
      hash_ *= 0x100000001b3ull;
    }
  }
  void Add(float value) { Add(uint64_t{std::bit_cast<uint32_t>(value)}); }
  void Add(bool value) { Add(uint64_t{value ? 1u : 0u}); }
  void Add(glm::vec3 value) {
    Add(value.x);
    Add(value.y);
    Add(value.z);
  }
  void Add(glm::quat value) {
    Add(value.w);
    Add(value.x);
    Add(value.y);
    Add(value.z);
  }

  [[nodiscard]] uint64_t value() const { return hash_; }

 private:
  uint64_t hash_ = 0xcbf29ce484222325ull;
};

void AddFirearm(Hasher& h, const weapon::FirearmState& f) {
  h.Add(uint64_t{static_cast<uint32_t>(f.fire_mode)});
  h.Add(static_cast<uint64_t>(static_cast<int64_t>(f.magazine)));
  h.Add(static_cast<uint64_t>(static_cast<int64_t>(f.incoming_magazine)));
  h.Add(f.magazine_inserted);
  h.Add(f.chambered);
  h.Add(f.bolt_locked);
  h.Add(f.empty_reload);
  h.Add(f.pull_used);
  h.Add(static_cast<uint64_t>(static_cast<int64_t>(f.reload_stage)));
  h.Add(f.reload_elapsed);
  h.Add(f.reload_duration);
  h.Add(f.cycle_remaining);
  for (const int16_t spare : f.spares) {
    h.Add(static_cast<uint64_t>(static_cast<int64_t>(spare)));
  }
  h.Add(uint64_t{f.spare_count});
  h.Add(uint64_t{f.spares_dropped});
}

void AddPlayer(Hasher& h, uint32_t id, const PlayerState& s) {
  h.Add(uint64_t{id});
  h.Add(s.position);
  h.Add(s.velocity);
  h.Add(s.jump_speed);
  h.Add(static_cast<uint64_t>(static_cast<int64_t>(s.jump_age)));
  h.Add(static_cast<uint64_t>(static_cast<int64_t>(s.jump_cooldown)));
  h.Add(s.jump_pending);
  h.Add(s.crouching);
  h.Add(s.on_ground);
  h.Add(s.yaw);
  h.Add(s.pitch);
  h.Add(s.eye_height);
  h.Add(s.sprinting);
  h.Add(s.aiming);
  h.Add(s.alive);
  h.Add(static_cast<uint64_t>(static_cast<int64_t>(s.health)));
  h.Add(uint64_t{s.respawn_ticks});
  h.Add(uint64_t{s.shots_fired});
  h.Add(uint64_t{s.deaths});
  const weapon::LoadoutState& l = s.loadout;
  h.Add(uint64_t{l.active ? static_cast<uint32_t>(*l.active) + 1 : 0u});
  h.Add(uint64_t{l.selected ? static_cast<uint32_t>(*l.selected) + 1 : 0u});
  h.Add(l.raise);
  h.Add(l.wait_for_release);
  for (size_t i = 0; i < l.slots.size(); ++i) {
    h.Add(l.slots[i].present);
    AddFirearm(h, l.slots[i].firearm);
    h.Add(l.slots[i].scope_magnification);
    h.Add(l.slots[i].scope_zero);
    h.Add(uint64_t{s.slot_keys[i]});
  }
}

// Hashes every read-out and event of the world after one tick.
void AddWorld(Hasher& h, SimWorld& world) {
  std::vector<PlayerReadout> players(4);
  const size_t player_count = world.ReadPlayers(players);
  REQUIRE(player_count <= players.size());
  for (size_t i = 0; i < player_count; ++i) {
    AddPlayer(h, players[i].id, players[i].state);
  }
  std::vector<BodyReadout> bodies(4);
  const size_t body_count = world.ReadBodies(bodies);
  REQUIRE(body_count <= bodies.size());
  for (size_t i = 0; i < body_count; ++i) {
    h.Add(uint64_t{bodies[i].instance});
    h.Add(bodies[i].state.pose.position);
    h.Add(bodies[i].state.pose.rotation);
    h.Add(bodies[i].state.linear_velocity);
    h.Add(bodies[i].state.angular_velocity);
    h.Add(bodies[i].state.active);
  }
  std::vector<DroppedReadout> dropped(8);
  const size_t dropped_count = world.ReadDroppedWeapons(dropped);
  REQUIRE(dropped_count <= dropped.size());
  for (size_t i = 0; i < dropped_count; ++i) {
    h.Add(uint64_t{dropped[i].id});
    h.Add(uint64_t{dropped[i].weapon_key});
    h.Add(dropped[i].pose.position);
    h.Add(dropped[i].pose.rotation);
    h.Add(dropped[i].linear_velocity);
    h.Add(dropped[i].angular_velocity);
    h.Add(static_cast<uint64_t>(static_cast<int64_t>(dropped[i].magazine)));
    h.Add(dropped[i].chambered);
    h.Add(dropped[i].active);
  }
  for (const SimEvent& event : testing::DrainEvents(world)) {
    h.Add(uint64_t{static_cast<uint32_t>(event.type)});
    h.Add(uint64_t{event.tick});
    h.Add(uint64_t{event.player});
    h.Add(uint64_t{event.other});
    h.Add(uint64_t{event.id});
    h.Add(uint64_t{event.weapon_key});
    h.Add(uint64_t{static_cast<uint8_t>(event.zone)});
    h.Add(event.chambered);
    h.Add(static_cast<uint64_t>(static_cast<int64_t>(event.value)));
    h.Add(static_cast<uint64_t>(static_cast<int64_t>(event.health)));
    h.Add(event.yaw);
    h.Add(event.position);
    h.Add(event.vector);
  }
}

// Returns a triangle wave of `tick` in [-1, 1] with `period` ticks, from
// integer arithmetic and one exact division: the script's angles must not
// depend on the platform's sin and cos.
float Triangle(uint32_t tick, uint32_t period) {
  const uint32_t phase = tick % period;
  const uint32_t half = period / 2;
  const auto rising =
      static_cast<float>(phase <= half ? phase : period - phase);
  return rising / static_cast<float>(half) * 2.0f - 1.0f;
}

// Player 1 runs around the crate, jumps, crouches and fires bursts down the
// range; player 2 strafes, sprints, reloads, switches weapons and fires at
// player 1's side of the range, drops its rifle and picks it up.
UserCommand Script(uint32_t player, uint32_t tick) {
  UserCommand c;
  c.tick = tick;
  if (player == 1) {
    c.yaw = 0.3f * Triangle(tick, 320);
    c.pitch = 0.05f * Triangle(tick + 50, 200);
    c.held |= (tick / 40) % 2 == 0 ? held::kForward : held::kBack;
    if (tick % 90 == 10) {
      c.pressed |= pressed::kJump;
    }
    if (tick >= 200 && tick < 260) {
      c.held |= held::kCrouch;
    }
    if (tick == 30) {
      c.pressed |= pressed::kFireMode;
    }
    if ((tick >= 40 && tick < 70) || (tick >= 300 && tick < 330)) {
      c.held |= held::kFire;
    }
    if (tick == 340) {
      c.pressed |= pressed::kReload;
    }
  } else {
    c.yaw = 3.1f - 0.2f * Triangle(tick, 420);
    c.pitch = -0.02f;
    c.held |= (tick / 50) % 2 == 0 ? held::kLeft : held::kRight;
    if (tick >= 100 && tick < 160) {
      c.held |= held::kSprint | held::kForward;
    }
    if (tick % 25 == 0 && tick < 450) {
      c.held |= held::kFire;
    }
    if (tick == 170) {
      c.pressed |= pressed::kReload;
    }
    if (tick == 260) {
      c.select_slot = 0;
    }
    if (tick == 380) {
      c.select_slot = 1;
    }
    if (tick == 470) {
      c.pitch = -1.5f;
      c.pressed |= pressed::kDropWeapon;
    }
    if (tick >= 520 && tick < 530) {
      c.held |= held::kAim;
    }
  }
  return c;
}

TEST_CASE("A fixed 600-tick game hashes to the checked-in constant") {
  const testing::TestScene files;
  physics::PhysicsRuntime runtime;
  auto created = SimWorld::Create(runtime, files.Options());
  REQUIRE(created.ok());
  SimWorld& world = *created.value();
  REQUIRE(world.AddPlayer(1));
  REQUIRE(world.AddPlayer(2));

  Hasher hasher;
  std::vector<uint64_t> tick_hashes;
  for (uint32_t tick = 1; tick <= kTicks; ++tick) {
    REQUIRE(world.ApplyCommand(1, Script(1, tick)));
    REQUIRE(world.ApplyCommand(2, Script(2, tick)));
    testing::RunTick(world, tick);
    AddWorld(hasher, world);
    tick_hashes.push_back(hasher.value());
  }
  // The script exercised what it should.
  CHECK(world.player(1)->Save().shots_fired > 0);
  CHECK(world.player(2)->Save().shots_fired > 0);
  MESSAGE(std::format("determinism hash: 0x{:016x}", hasher.value()));
  CHECK(hasher.value() == kExpectedHash);
  if (hasher.value() != kExpectedHash) {
    // Compare these lines between platforms to find the first divergence.
    for (size_t i = 0; i < tick_hashes.size(); ++i) {
      std::printf("tick %zu hash %016llx\n", i + 1,
                  static_cast<unsigned long long>(tick_hashes[i]));
    }
  }
}

}  // namespace
}  // namespace blue::sim
