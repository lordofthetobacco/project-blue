#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>

#include <glm/vec3.hpp>

#include "net/bit_stream.h"
#include "net/players.h"
#include "net/protocol.h"
#include "net/state_mapping.h"
#include "sim/player_state.h"
#include "weapon/firearm.h"
#include "weapon/loadout.h"

namespace blue::net {
namespace {

// Exact equality of every field (sim::PlayerState defines no operator==).
bool Same(const sim::PlayerState& a, const sim::PlayerState& b) {
  return a.position == b.position && a.velocity == b.velocity &&
         a.jump_speed == b.jump_speed && a.jump_age == b.jump_age &&
         a.jump_cooldown == b.jump_cooldown &&
         a.jump_pending == b.jump_pending && a.crouching == b.crouching &&
         a.on_ground == b.on_ground && a.yaw == b.yaw && a.pitch == b.pitch &&
         a.eye_height == b.eye_height && a.sprinting == b.sprinting &&
         a.aiming == b.aiming && a.alive == b.alive && a.health == b.health &&
         a.respawn_ticks == b.respawn_ticks && a.shots_fired == b.shots_fired &&
         a.deaths == b.deaths && a.loadout == b.loadout &&
         a.slot_keys == b.slot_keys;
}

// A state with every field off its default.
sim::PlayerState BusyState() {
  sim::PlayerState state;
  state.position = glm::vec3(1.5f, 0.25f, -7.0f);
  state.velocity = glm::vec3(3.0f, -1.0f, 0.5f);
  state.jump_speed = 4.75f;
  state.jump_age = 3;
  state.jump_cooldown = 7;
  state.jump_pending = true;
  state.crouching = true;
  state.on_ground = true;
  state.yaw = 2.5f;
  state.pitch = -0.25f;
  state.eye_height = 1.2f;
  state.sprinting = true;
  state.aiming = true;
  state.alive = true;
  state.health = 64;
  state.respawn_ticks = 90;
  state.shots_fired = 4321;
  state.deaths = 5;
  state.loadout.active = weapon::WeaponSlot::kRifle;
  state.loadout.selected = weapon::WeaponSlot::kPistol;
  state.loadout.raise = 0.375f;
  state.loadout.wait_for_release = true;
  weapon::LoadoutSlotState& rifle = state.loadout.slots[1];
  rifle.present = true;
  rifle.scope_magnification = 4.0f;
  rifle.scope_zero = 300.0f;
  rifle.firearm.fire_mode = weapon::FireMode::kAuto;
  rifle.firearm.magazine = 17;
  rifle.firearm.incoming_magazine = 30;
  rifle.firearm.magazine_inserted = false;
  rifle.firearm.chambered = true;
  rifle.firearm.bolt_locked = true;
  rifle.firearm.empty_reload = true;
  rifle.firearm.pull_used = true;
  rifle.firearm.reload_stage = 2;
  rifle.firearm.reload_elapsed = 0.5f;
  rifle.firearm.reload_duration = 2.2f;
  rifle.firearm.cycle_remaining = 0.04f;
  rifle.firearm.spares = {30, 30, 12, 0, 0, 0, 0, 0};
  rifle.firearm.spare_count = 3;
  rifle.firearm.spares_dropped = 2;
  state.slot_keys = {0, 0xCAFEF00Du};
  return state;
}

TEST_CASE("player state maps to the wire and back exactly") {
  const sim::PlayerState state = BusyState();
  const WirePlayerState wire = ToWire(state);
  CHECK(wire.flags == (player_flags::kCrouching | player_flags::kSprinting |
                       player_flags::kAiming | player_flags::kAlive |
                       player_flags::kOnGround | player_flags::kJumpPending));
  CHECK(wire.active_slot == 1);
  CHECK(wire.selected_slot == 0);
  CHECK_FALSE(wire.slots[0].present);
  CHECK(wire.slots[0] == WireWeaponSlot{});
  CHECK(wire.slots[1].weapon_key == 0xCAFEF00Du);
  CHECK(wire.slots[1].incoming_magazine == 30);
  CHECK(Same(FromWire(wire), state));
}

TEST_CASE("player state survives the wire codec") {
  const sim::PlayerState state = BusyState();
  std::array<uint8_t, kMaxPayloadBytes> buffer{};
  BitWriter writer(buffer);
  WritePlayerState(ToWire(state), writer);
  REQUIRE(writer.ok());
  BitReader reader(std::span<const uint8_t>(buffer.data(), buffer.size()));
  const WirePlayerState decoded = ReadPlayerState(reader);
  REQUIRE(reader.ok());
  CHECK(Same(FromWire(decoded), state));
}

TEST_CASE("default and empty-handed states round-trip") {
  const sim::PlayerState fresh;
  CHECK(Same(FromWire(ToWire(fresh)), fresh));
  sim::PlayerState dead = BusyState();
  dead.alive = false;
  dead.loadout = weapon::LoadoutState{};
  dead.slot_keys = {};
  CHECK(ToWire(dead).active_slot == -1);
  CHECK(Same(FromWire(ToWire(dead)), dead));
}

TEST_CASE("wide integers saturate on the wire") {
  sim::PlayerState state = BusyState();
  state.jump_age = 1000;
  state.jump_cooldown = -4;
  state.loadout.slots[1].firearm.magazine = 300;
  state.loadout.slots[1].firearm.spares[0] = 999;
  state.loadout.slots[1].firearm.incoming_magazine = -7;
  const WirePlayerState wire = ToWire(state);
  CHECK(wire.jump_age == 255);
  CHECK(wire.jump_cooldown == 0);
  CHECK(wire.slots[1].magazine_rounds == 255);
  CHECK(wire.slots[1].spare_magazines[0] == 255);
  CHECK(wire.slots[1].incoming_magazine == -1);
}

TEST_CASE("unknown slot indices and fire modes are clamped") {
  WirePlayerState wire = ToWire(BusyState());
  wire.active_slot = 5;
  wire.selected_slot = 7;
  wire.slots[1].fire_mode = 9;
  const sim::PlayerState state = FromWire(wire);
  CHECK_FALSE(state.loadout.active.has_value());
  CHECK_FALSE(state.loadout.selected.has_value());
  CHECK(state.loadout.slots[1].firearm.fire_mode ==
        weapon::FireMode::kDoubleAction);
}

}  // namespace
}  // namespace blue::net
