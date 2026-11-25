#ifndef BLUE_SIM_PLAYER_SIM_H_
#define BLUE_SIM_PLAYER_SIM_H_

#include <array>
#include <cstdint>
#include <memory>
#include <optional>

#include <glm/vec3.hpp>

#include "camera/look.h"
#include "core/result.h"
#include "physics/character.h"
#include "physics/physics_world.h"
#include "player/player_controller.h"
#include "sim/player_state.h"
#include "sim/user_command.h"
#include "sim/weapon_table.h"
#include "weapon/firearm.h"
#include "weapon/loadout.h"
#include "weapon/weapon_def.h"
#include "weapon/weapon_item.h"

namespace blue::sim {

// One bullet a player fired during a tick. The bullet leaves the sim eye
// along `direction` at `muzzle_velocity` plus the shooter's velocity
// (`velocity` holds the sum).
struct ShotEvent {
  glm::vec3 origin{0.0f};        // the sim eye: feet + eye height
  glm::vec3 direction{0.0f};     // unit: zeroed and dispersed look direction
  float muzzle_velocity = 0.0f;  // m/s
  glm::vec3 shooter_velocity{0.0f};  // m/s
  glm::vec3 velocity{0.0f};          // m/s, the bullet's initial velocity
  float mass = 0.0f;                 // kg
  float drag = 0.0f;                 // 1/m
  // s between the shot and the end of the tick (weapon::FirearmEvents).
  float age = 0.0f;
  uint32_t shot_number = 0;  // PlayerState::shots_fired after this shot
  WeaponKey weapon_key = 0;
};

// What one PlayerSim::Step did.
struct StepEvents {
  std::array<ShotEvent, weapon::kMaxShotsPerUpdate> shots{};
  int shot_count = 0;  // valid entries of `shots`, in firing order
  bool dry_fire = false;
  bool reload_started = false;
  bool reload_finished = false;
  bool fire_mode_changed = false;
  bool weapon_changed = false;  // weapon::LoadoutEvents::weapon_changed
};

// Returns the dispersion seed of shot `shot_number` of player `player_id`: a
// mix of both, never 0. The shot sequence rather than the tick decides the
// spread, so a prediction one tick off still spreads the same.
[[nodiscard]] uint32_t ShotSeed(uint32_t player_id, uint32_t shot_number);

// One player of the simulation: a manually stepped physics::Character, the
// view (angles, blended eye height), life and a weapon::Loadout, advanced in
// whole ticks from UserCommands. The same code runs the server's authority
// and the client's prediction; the client replays by Restore() of an older
// tick and Step() of the commands since, which never steps rigid bodies and
// works against a world whose dynamic bodies are kinematic.
//
// Step() runs, in order: the stance (crouch request), player::ComputeMove,
// Character::Step, the eye height blend, the loadout and firearm update over
// kTickSeconds (weapon selection, reload, fire mode, trigger; no firing while
// sprinting), one ShotEvent per shot, then the zero keys (which apply from
// the next shot on). A dead player ignores commands and does not move.
//
// Deaths, respawns, damage and weapons changing hands are the server's
// business (SimWorld) through the mutators below; the client learns of them
// from the authoritative state.
//
// Borrows `world`, `library` and `table`, which must outlive it; owns its
// character, so it must be destroyed before the world. No allocation per
// tick. Neither copyable nor movable. Not thread-safe.
class PlayerSim {
 public:
  // Creates player `player_id` standing with its feet at `feet`, looking
  // along `look`, carrying `loadout` (weapons of `library`, all known to
  // `table`). Fails like physics::Character::Create. `character` is used
  // with manual_step forced on.
  static core::Result<std::unique_ptr<PlayerSim>, physics::PhysicsError> Create(
      physics::PhysicsWorld& world, const weapon::WeaponLibrary& library,
      const WeaponTable& table, uint32_t player_id, glm::vec3 feet,
      camera::LookAngles look, weapon::Loadout loadout,
      const physics::CharacterDesc& character = {},
      const player::PlayerTuning& tuning = {});

  PlayerSim(const PlayerSim&) = delete;
  PlayerSim& operator=(const PlayerSim&) = delete;
  ~PlayerSim();

  // Returns the state after the last Step (or Restore, or creation).
  [[nodiscard]] PlayerState Save() const;
  // Puts the player into `state`. A loadout carrying other weapons than
  // `state` names is replaced by fresh weapons of the named keys first, then
  // restored. Returns false, leaving the player partly restored, when a key
  // is unknown to the table or the loadout state does not apply.
  [[nodiscard]] bool Restore(const PlayerState& state);
  // Advances the player by one tick under `command`; see the class comment.
  StepEvents Step(const UserCommand& command);

  // Server mutators.

  // Throws the weapon in hand away (weapon::Loadout::Drop): nullopt with
  // empty hands or while it is being put away.
  std::optional<weapon::WeaponItem> DropActiveWeapon();
  // Takes `weapon` into its slot (weapon::Loadout::PickUp).
  weapon::PickUpResult PickUp(weapon::WeaponItem weapon);
  // Sets the health, clamped to [0, kMaxHealth]. Does not kill.
  void SetHealth(int health);
  // Kills the player: not alive, `respawn_ticks` until the respawn, one more
  // death. Does nothing to a dead player.
  void Kill(uint16_t respawn_ticks);
  // Counts one tick of a dead player's respawn timer down; returns true when
  // it has run out (also for a timer already at 0). False for a live player.
  bool CountDownRespawn();
  // Brings the player back alive at `feet` looking along `look`, standing,
  // at rest, with full health and `loadout`.
  void Respawn(glm::vec3 feet, camera::LookAngles look,
               weapon::Loadout loadout);

  [[nodiscard]] uint32_t id() const { return id_; }
  [[nodiscard]] bool alive() const { return alive_; }
  [[nodiscard]] int health() const { return health_; }
  [[nodiscard]] camera::LookAngles look() const { return look_; }
  // Feet after the last step.
  [[nodiscard]] glm::vec3 feet() const;
  [[nodiscard]] glm::vec3 velocity() const;
  // The sim eye: feet plus the blended eye height.
  [[nodiscard]] glm::vec3 eye() const;
  // Height and radius of the current capsule (m).
  [[nodiscard]] float capsule_height() const;
  [[nodiscard]] float capsule_radius() const;
  [[nodiscard]] const weapon::Loadout& loadout() const { return loadout_; }
  [[nodiscard]] const player::PlayerTuning& tuning() const { return tuning_; }

 private:
  PlayerSim(const weapon::WeaponLibrary& library, const WeaponTable& table,
            uint32_t player_id, camera::LookAngles look,
            weapon::Loadout loadout, const player::PlayerTuning& tuning,
            std::unique_ptr<physics::Character> character);

  // Returns the table key of the weapon in `slot`, 0 for an empty slot.
  [[nodiscard]] WeaponKey KeyOf(weapon::WeaponSlot slot) const;
  // Makes the loadout carry fresh weapons of `keys` (by slot, `present` per
  // `state`). Returns false for an unknown key.
  [[nodiscard]] bool RebuildLoadout(const PlayerState& state);
  // Returns the zero distance the weapon in hand is sighted at.
  [[nodiscard]] float ZeroDistance(const weapon::WeaponItem& item) const;
  // Returns the zeroing angle of the weapon in hand (cached per weapon and
  // zero distance).
  [[nodiscard]] float ZeroAngle(const weapon::WeaponItem& item);
  // Appends a ShotEvent for one shot of the weapon in hand.
  void AddShot(float age, StepEvents& events);
  // Applies the zero keys of `command` to the weapon in hand.
  void ApplyZeroKeys(const UserCommand& command);

  const weapon::WeaponLibrary& library_;
  const WeaponTable& table_;
  uint32_t id_ = 0;
  player::PlayerTuning tuning_;
  std::unique_ptr<physics::Character> character_;
  weapon::Loadout loadout_;
  camera::LookAngles look_;
  float eye_height_ = 0.0f;
  // Share of the gap to the stance's eye height closed per tick.
  float eye_blend_ = 0.0f;
  bool sprinting_ = false;
  bool aiming_ = false;
  bool alive_ = true;
  int16_t health_ = kMaxHealth;
  uint16_t respawn_ticks_ = 0;
  uint32_t shots_fired_ = 0;
  uint32_t deaths_ = 0;
  // ZeroAngle() cache: the definition and distance it was computed for.
  std::optional<weapon::WeaponDefId> zero_def_;
  float zero_distance_ = 0.0f;
  float zero_angle_ = 0.0f;
};

}  // namespace blue::sim

#endif  // BLUE_SIM_PLAYER_SIM_H_
