#ifndef BLUE_SIM_PLAYER_STATE_H_
#define BLUE_SIM_PLAYER_STATE_H_

#include <array>
#include <cstdint>
#include <type_traits>

#include <glm/vec3.hpp>

#include "sim/weapon_table.h"
#include "weapon/loadout.h"

namespace blue::sim {

// Hit points of a player at full health.
inline constexpr int16_t kMaxHealth = 100;

// Everything of one simulated player that carries from one tick to the next:
// the state PlayerSim::Save() makes and PlayerSim::Restore() applies, the
// client's per-tick prediction history and the server's authoritative copy.
// Plain data (trivially copyable, fixed size). The wire protocol's own player
// state mirrors it field by field.
struct PlayerState {
  // Character (physics::CharacterState).
  glm::vec3 position{0.0f};   // feet, m
  glm::vec3 velocity{0.0f};   // m/s
  float jump_speed = 0.0f;    // m/s of the pending (or last) jump request
  int32_t jump_age = 0;       // ticks since the pending jump request
  int32_t jump_cooldown = 0;  // ticks before the character counts as grounded
  bool jump_pending = false;
  bool crouching = false;
  // Standing on walkable ground after the tick. Compared, never restored:
  // the character finds it again from its contacts.
  bool on_ground = false;

  // View.
  float yaw = 0.0f;    // radians, from the command
  float pitch = 0.0f;  // radians, from the command
  // m above the feet; blends towards the stance's eye height every tick.
  float eye_height = 0.0f;
  bool sprinting = false;
  bool aiming = false;

  // Life.
  bool alive = true;
  int16_t health = kMaxHealth;  // [0, kMaxHealth]
  uint16_t respawn_ticks = 0;   // ticks until a dead player respawns
  uint32_t shots_fired = 0;     // every shot since joining; seeds dispersion
  uint32_t deaths = 0;          // picks the respawn point

  // Weapons: the loadout's state and, per weapon::WeaponSlot, the key of the
  // weapon in the slot (0 for an empty slot).
  weapon::LoadoutState loadout;
  std::array<WeaponKey, weapon::kWeaponSlotCount> slot_keys{};
};

static_assert(std::is_trivially_copyable_v<PlayerState>);

// Reconcile thresholds of NeedsReplay().
inline constexpr float kReplayPositionTolerance = 0.01f;  // m
inline constexpr float kReplayVelocityTolerance = 0.1f;   // m/s
// m of eye height and m/s of jump speed (both blend or copy deterministically).
inline constexpr float kReplayScalarTolerance = 1.0e-3f;

// Returns whether a client whose prediction for a tick was `predicted` must
// restore `authoritative` and replay: the feet are more than
// kReplayPositionTolerance apart, the velocities more than
// kReplayVelocityTolerance, the eye height or jump speed more than
// kReplayScalarTolerance, the loadouts differ (weapon::NearlyEqual, firearm
// timers within 1 ms), or any flag or integer differs. The view angles are
// not compared: they come from the command.
[[nodiscard]] bool NeedsReplay(const PlayerState& predicted,
                               const PlayerState& authoritative);

}  // namespace blue::sim

#endif  // BLUE_SIM_PLAYER_STATE_H_
