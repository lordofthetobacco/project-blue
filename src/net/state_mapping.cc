#include "net/state_mapping.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "net/players.h"
#include "sim/player_state.h"
#include "sim/weapon_table.h"
#include "weapon/firearm.h"
#include "weapon/loadout.h"

namespace blue::net {
namespace {

// Returns `value` clamped to 0..255.
uint8_t SaturateU8(int value) {
  return static_cast<uint8_t>(std::clamp(value, 0, 255));
}

int8_t SlotIndex(std::optional<weapon::WeaponSlot> slot) {
  return slot.has_value() ? static_cast<int8_t>(*slot) : int8_t{-1};
}

// Returns the slot of wire index `index`; nullopt for -1 and for indices
// the loadout does not have.
std::optional<weapon::WeaponSlot> SlotOf(int8_t index) {
  if (index < 0 || index >= weapon::kWeaponSlotCount) {
    return std::nullopt;
  }
  return static_cast<weapon::WeaponSlot>(index);
}

uint8_t FlagsOf(const sim::PlayerState& state) {
  uint8_t flags = 0;
  const auto set = [&flags](bool on, uint8_t bit) {
    if (on) {
      flags |= bit;
    }
  };
  set(state.crouching, player_flags::kCrouching);
  set(state.sprinting, player_flags::kSprinting);
  set(state.aiming, player_flags::kAiming);
  set(state.alive, player_flags::kAlive);
  set(state.on_ground, player_flags::kOnGround);
  set(state.jump_pending, player_flags::kJumpPending);
  return flags;
}

WireWeaponSlot SlotToWire(const weapon::LoadoutSlotState& slot,
                          sim::WeaponKey key) {
  WireWeaponSlot wire;
  if (!slot.present) {
    return wire;
  }
  const weapon::FirearmState& firearm = slot.firearm;
  wire.present = true;
  wire.weapon_key = key;
  wire.fire_mode = static_cast<uint8_t>(firearm.fire_mode);
  wire.magazine_rounds = SaturateU8(firearm.magazine);
  wire.magazine_inserted = firearm.magazine_inserted;
  wire.chambered = firearm.chambered;
  wire.bolt_locked = firearm.bolt_locked;
  wire.empty_reload = firearm.empty_reload;
  wire.reload_stage = static_cast<uint8_t>(
      std::clamp<int>(firearm.reload_stage, 0, kMaxReloadStage));
  wire.reload_elapsed = firearm.reload_elapsed;
  wire.reload_duration = firearm.reload_duration;
  wire.cycle_remaining = firearm.cycle_remaining;
  wire.spare_count = static_cast<uint8_t>(
      std::min<int>(firearm.spare_count, kMaxSpareMagazines));
  for (size_t i = 0; i < wire.spare_count; ++i) {
    wire.spare_magazines[i] = SaturateU8(firearm.spares[i]);
  }
  wire.incoming_magazine = static_cast<int16_t>(std::clamp<int>(
      firearm.incoming_magazine, kMinIncomingMagazine, kMaxIncomingMagazine));
  wire.pull_used = firearm.pull_used;
  wire.scope_magnification = slot.scope_magnification;
  wire.scope_zero = slot.scope_zero;
  wire.spares_dropped = firearm.spares_dropped;
  return wire;
}

weapon::LoadoutSlotState SlotFromWire(const WireWeaponSlot& wire) {
  weapon::LoadoutSlotState slot;
  if (!wire.present) {
    return slot;
  }
  weapon::FirearmState& firearm = slot.firearm;
  slot.present = true;
  slot.scope_magnification = wire.scope_magnification;
  slot.scope_zero = wire.scope_zero;
  firearm.fire_mode = static_cast<weapon::FireMode>(
      std::min<int>(wire.fire_mode, weapon::kFireModeCount - 1));
  firearm.magazine = wire.magazine_rounds;
  firearm.incoming_magazine = wire.incoming_magazine;
  firearm.magazine_inserted = wire.magazine_inserted;
  firearm.chambered = wire.chambered;
  firearm.bolt_locked = wire.bolt_locked;
  firearm.empty_reload = wire.empty_reload;
  firearm.pull_used = wire.pull_used;
  firearm.reload_stage = static_cast<int8_t>(wire.reload_stage);
  firearm.reload_elapsed = wire.reload_elapsed;
  firearm.reload_duration = wire.reload_duration;
  firearm.cycle_remaining = wire.cycle_remaining;
  const size_t spares =
      std::min<size_t>(std::min<size_t>(wire.spare_count, kMaxSpareMagazines),
                       firearm.spares.size());
  for (size_t i = 0; i < spares; ++i) {
    firearm.spares[i] = wire.spare_magazines[i];
  }
  firearm.spare_count = static_cast<uint8_t>(spares);
  firearm.spares_dropped = wire.spares_dropped;
  return slot;
}

}  // namespace

WirePlayerState ToWire(const sim::PlayerState& state) {
  WirePlayerState wire;
  wire.position = state.position;
  wire.velocity = state.velocity;
  wire.yaw = state.yaw;
  wire.pitch = state.pitch;
  wire.eye_height = state.eye_height;
  wire.flags = FlagsOf(state);
  wire.jump_age = SaturateU8(state.jump_age);
  wire.jump_cooldown = SaturateU8(state.jump_cooldown);
  wire.health = state.health;
  wire.respawn_ticks = state.respawn_ticks;
  wire.shots_fired = state.shots_fired;
  wire.active_slot = SlotIndex(state.loadout.active);
  wire.selected_slot = SlotIndex(state.loadout.selected);
  wire.raise = state.loadout.raise;
  wire.jump_speed = state.jump_speed;
  wire.deaths = state.deaths;
  wire.wait_for_release = state.loadout.wait_for_release;
  for (size_t i = 0; i < wire.slots.size(); ++i) {
    wire.slots[i] = SlotToWire(state.loadout.slots[i], state.slot_keys[i]);
  }
  return wire;
}

sim::PlayerState FromWire(const WirePlayerState& wire) {
  sim::PlayerState state;
  state.position = wire.position;
  state.velocity = wire.velocity;
  state.jump_speed = wire.jump_speed;
  state.jump_age = wire.jump_age;
  state.jump_cooldown = wire.jump_cooldown;
  state.jump_pending = (wire.flags & player_flags::kJumpPending) != 0;
  state.crouching = (wire.flags & player_flags::kCrouching) != 0;
  state.on_ground = (wire.flags & player_flags::kOnGround) != 0;
  state.yaw = wire.yaw;
  state.pitch = wire.pitch;
  state.eye_height = wire.eye_height;
  state.sprinting = (wire.flags & player_flags::kSprinting) != 0;
  state.aiming = (wire.flags & player_flags::kAiming) != 0;
  state.alive = (wire.flags & player_flags::kAlive) != 0;
  state.health = wire.health;
  state.respawn_ticks = wire.respawn_ticks;
  state.shots_fired = wire.shots_fired;
  state.deaths = wire.deaths;
  state.loadout.active = SlotOf(wire.active_slot);
  state.loadout.selected = SlotOf(wire.selected_slot);
  state.loadout.raise = wire.raise;
  state.loadout.wait_for_release = wire.wait_for_release;
  for (size_t i = 0; i < state.loadout.slots.size(); ++i) {
    state.loadout.slots[i] = SlotFromWire(wire.slots[i]);
    state.slot_keys[i] = wire.slots[i].present ? wire.slots[i].weapon_key : 0;
  }
  return state;
}

}  // namespace blue::net
