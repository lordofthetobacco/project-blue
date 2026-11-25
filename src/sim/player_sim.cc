#include "sim/player_sim.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

#include "camera/look.h"
#include "core/det_math.h"
#include "core/result.h"
#include "physics/character.h"
#include "physics/physics_world.h"
#include "player/movement.h"
#include "player/player_controller.h"
#include "sim/player_state.h"
#include "sim/user_command.h"
#include "sim/weapon_table.h"
#include "weapon/dispersion.h"
#include "weapon/firearm.h"
#include "weapon/loadout.h"
#include "weapon/parts/weapon_tuning.h"
#include "weapon/scope.h"
#include "weapon/viewmodel.h"
#include "weapon/weapon_def.h"
#include "weapon/weapon_item.h"

namespace blue::sim {
namespace {

// The view pitch limit of camera::LookAngles: +-89 degrees.
constexpr float kMaxPitch = 1.5533430f;

// Returns the share of the remaining gap an exponential approach at `rate`
// (1/s) closes in one tick (core::DetExp: the same on every platform).
float TickBlend(float rate) {
  return 1.0f - core::DetExp(-rate * kTickSeconds);
}

bool Has(uint32_t bits, uint32_t mask) { return (bits & mask) != 0; }

// Returns the slot a command selects, or nullopt.
std::optional<weapon::WeaponSlot> SelectedSlot(const UserCommand& command) {
  if (command.select_slot < 0 ||
      command.select_slot >= weapon::kWeaponSlotCount) {
    return std::nullopt;
  }
  return static_cast<weapon::WeaponSlot>(command.select_slot);
}

bool Differ(glm::vec3 a, glm::vec3 b, float tolerance) {
  return glm::length(a - b) > tolerance;
}

}  // namespace

bool NeedsReplay(const PlayerState& predicted,
                 const PlayerState& authoritative) {
  const PlayerState& p = predicted;
  const PlayerState& a = authoritative;
  if (Differ(p.position, a.position, kReplayPositionTolerance) ||
      Differ(p.velocity, a.velocity, kReplayVelocityTolerance) ||
      std::abs(p.eye_height - a.eye_height) > kReplayScalarTolerance ||
      std::abs(p.jump_speed - a.jump_speed) > kReplayScalarTolerance) {
    return true;
  }
  if (p.jump_age != a.jump_age || p.jump_cooldown != a.jump_cooldown ||
      p.jump_pending != a.jump_pending || p.crouching != a.crouching ||
      p.on_ground != a.on_ground || p.sprinting != a.sprinting ||
      p.aiming != a.aiming || p.alive != a.alive || p.health != a.health ||
      p.respawn_ticks != a.respawn_ticks || p.shots_fired != a.shots_fired ||
      p.deaths != a.deaths || p.slot_keys != a.slot_keys) {
    return true;
  }
  return !weapon::NearlyEqual(p.loadout, a.loadout);
}

uint32_t ShotSeed(uint32_t player_id, uint32_t shot_number) {
  // murmur3's 32-bit finaliser over a combination of both.
  uint32_t h = player_id * 0x9e3779b1u ^ (shot_number + 0x7f4a7c15u);
  h ^= h >> 16;
  h *= 0x85ebca6bu;
  h ^= h >> 13;
  h *= 0xc2b2ae35u;
  h ^= h >> 16;
  return h != 0 ? h : 1u;
}

core::Result<std::unique_ptr<PlayerSim>, physics::PhysicsError>
PlayerSim::Create(physics::PhysicsWorld& world,
                  const weapon::WeaponLibrary& library,
                  const WeaponTable& table, uint32_t player_id, glm::vec3 feet,
                  camera::LookAngles look, weapon::Loadout loadout,
                  const physics::CharacterDesc& character,
                  const player::PlayerTuning& tuning) {
  using CreateResult =
      core::Result<std::unique_ptr<PlayerSim>, physics::PhysicsError>;
  physics::CharacterDesc desc = character;
  desc.manual_step = true;
  core::Result<std::unique_ptr<physics::Character>, physics::PhysicsError>
      created = physics::Character::Create(world, desc, feet);
  if (!created.ok()) {
    return CreateResult::Error(created.error());
  }
  return CreateResult::Ok(std::unique_ptr<PlayerSim>(
      new PlayerSim(library, table, player_id, look, std::move(loadout), tuning,
                    std::move(created).value())));
}

PlayerSim::PlayerSim(const weapon::WeaponLibrary& library,
                     const WeaponTable& table, uint32_t player_id,
                     camera::LookAngles look, weapon::Loadout loadout,
                     const player::PlayerTuning& tuning,
                     std::unique_ptr<physics::Character> character)
    : library_(library),
      table_(table),
      id_(player_id),
      tuning_(tuning),
      character_(std::move(character)),
      loadout_(std::move(loadout)),
      look_(look),
      eye_height_(tuning.eye_height_standing),
      eye_blend_(TickBlend(tuning.stance_blend_rate)) {}

PlayerSim::~PlayerSim() = default;

PlayerState PlayerSim::Save() const {
  const physics::CharacterState character = character_->SaveState();
  PlayerState state;
  state.position = character.position;
  state.velocity = character.velocity;
  state.jump_speed = character.jump_speed;
  state.jump_age = character.jump_age;
  state.jump_cooldown = character.jump_cooldown;
  state.jump_pending = character.jump_pending;
  state.crouching = character.crouching;
  state.on_ground = character_->IsOnGround();
  state.yaw = look_.yaw;
  state.pitch = look_.pitch;
  state.eye_height = eye_height_;
  state.sprinting = sprinting_;
  state.aiming = aiming_;
  state.alive = alive_;
  state.health = health_;
  state.respawn_ticks = respawn_ticks_;
  state.shots_fired = shots_fired_;
  state.deaths = deaths_;
  state.loadout = loadout_.SaveState();
  for (int i = 0; i < weapon::kWeaponSlotCount; ++i) {
    state.slot_keys[static_cast<size_t>(i)] =
        KeyOf(static_cast<weapon::WeaponSlot>(i));
  }
  return state;
}

bool PlayerSim::Restore(const PlayerState& state) {
  character_->RestoreState({.position = state.position,
                            .velocity = state.velocity,
                            .jump_pending = state.jump_pending,
                            .jump_age = state.jump_age,
                            .jump_cooldown = state.jump_cooldown,
                            .crouching = state.crouching,
                            .jump_speed = state.jump_speed});
  look_ = {.yaw = state.yaw, .pitch = state.pitch};
  eye_height_ = state.eye_height;
  sprinting_ = state.sprinting;
  aiming_ = state.aiming;
  alive_ = state.alive;
  health_ = std::clamp<int16_t>(state.health, 0, kMaxHealth);
  respawn_ticks_ = state.respawn_ticks;
  shots_fired_ = state.shots_fired;
  deaths_ = state.deaths;

  bool same_weapons = true;
  for (int i = 0; i < weapon::kWeaponSlotCount; ++i) {
    const auto slot = static_cast<weapon::WeaponSlot>(i);
    const auto index = static_cast<size_t>(i);
    const bool present = state.loadout.slots[index].present;
    if (present != (loadout_.item(slot) != nullptr) ||
        (present && state.slot_keys[index] != KeyOf(slot))) {
      same_weapons = false;
    }
  }
  if (!same_weapons && !RebuildLoadout(state)) {
    return false;
  }
  return loadout_.RestoreState(state.loadout);
}

StepEvents PlayerSim::Step(const UserCommand& command) {
  StepEvents events;
  if (!alive_) {
    return events;
  }
  // Angles come from the network: non-finite ones keep the previous look.
  if (std::isfinite(command.yaw) && std::isfinite(command.pitch)) {
    look_ = {.yaw = command.yaw,
             .pitch = std::clamp(command.pitch, -kMaxPitch, kMaxPitch)};
  }

  const bool want_crouch = Has(command.held, buttons::held::kCrouch);
  if (want_crouch != character_->crouching()) {
    (void)character_->SetCrouching(want_crouch);
  }
  const player::MoveIntent intent = player::ComputeMove(
      {.forward = Has(command.held, buttons::held::kForward),
       .back = Has(command.held, buttons::held::kBack),
       .left = Has(command.held, buttons::held::kLeft),
       .right = Has(command.held, buttons::held::kRight),
       .sprint = Has(command.held, buttons::held::kSprint),
       .aim = Has(command.held, buttons::held::kAim),
       .jump = Has(command.pressed, buttons::pressed::kJump),
       .crouching = character_->crouching(),
       .yaw = look_.yaw},
      tuning_);
  sprinting_ = intent.sprinting;
  aiming_ = intent.aiming;
  character_->SetDesiredVelocity(intent.velocity);
  if (intent.jump) {
    character_->RequestJump(tuning_.jump_speed);
  }
  character_->Step();

  const float target_eye = character_->crouching()
                               ? tuning_.eye_height_crouching
                               : tuning_.eye_height_standing;
  eye_height_ += (target_eye - eye_height_) * eye_blend_;

  const weapon::LoadoutEvents loadout_events = loadout_.Update(
      {.select = SelectedSlot(command),
       .firearm = {.trigger_held = Has(command.held, buttons::held::kFire),
                   .reload_pressed =
                       Has(command.pressed, buttons::pressed::kReload),
                   .fire_mode_pressed =
                       Has(command.pressed, buttons::pressed::kFireMode),
                   .can_fire = !sprinting_}},
      kTickSeconds);
  const weapon::FirearmEvents& firearm = loadout_events.firearm;
  events.dry_fire = firearm.dry_fire;
  events.reload_started = firearm.reload_started;
  events.reload_finished = firearm.reload_finished;
  events.fire_mode_changed = firearm.fire_mode_changed;
  events.weapon_changed = loadout_events.weapon_changed;
  if (loadout_.active_item() != nullptr) {
    const int shots = std::min(firearm.shots, weapon::kMaxShotsPerUpdate);
    for (int i = 0; i < shots; ++i) {
      AddShot(firearm.shot_age[static_cast<size_t>(i)], events);
    }
  }
  ApplyZeroKeys(command);
  return events;
}

std::optional<weapon::WeaponItem> PlayerSim::DropActiveWeapon() {
  return loadout_.Drop();
}

weapon::PickUpResult PlayerSim::PickUp(weapon::WeaponItem weapon) {
  return loadout_.PickUp(std::move(weapon));
}

void PlayerSim::SetHealth(int health) {
  health_ = static_cast<int16_t>(std::clamp<int>(health, 0, kMaxHealth));
}

void PlayerSim::Kill(uint16_t respawn_ticks) {
  if (!alive_) {
    return;
  }
  alive_ = false;
  health_ = 0;
  respawn_ticks_ = respawn_ticks;
  ++deaths_;
  sprinting_ = false;
  aiming_ = false;
  character_->SetDesiredVelocity(glm::vec3(0.0f));
}

bool PlayerSim::CountDownRespawn() {
  if (alive_) {
    return false;
  }
  if (respawn_ticks_ > 0) {
    --respawn_ticks_;
  }
  return respawn_ticks_ == 0;
}

void PlayerSim::Respawn(glm::vec3 feet, camera::LookAngles look,
                        weapon::Loadout loadout) {
  character_->RestoreState({.position = feet});
  look_ = look;
  eye_height_ = tuning_.eye_height_standing;
  sprinting_ = false;
  aiming_ = false;
  alive_ = true;
  health_ = kMaxHealth;
  respawn_ticks_ = 0;
  loadout_ = std::move(loadout);
  zero_def_.reset();
}

glm::vec3 PlayerSim::feet() const { return character_->tick_position(); }

glm::vec3 PlayerSim::velocity() const { return character_->velocity(); }

glm::vec3 PlayerSim::eye() const {
  return feet() + camera::kWorldUp * eye_height_;
}

float PlayerSim::capsule_height() const { return character_->height(); }

float PlayerSim::capsule_radius() const { return character_->desc().radius; }

WeaponKey PlayerSim::KeyOf(weapon::WeaponSlot slot) const {
  const weapon::WeaponItem* item = loadout_.item(slot);
  if (item == nullptr) {
    return 0;
  }
  return table_.KeyOf(item->def).value_or(0);
}

bool PlayerSim::RebuildLoadout(const PlayerState& state) {
  // Allocates, but only when the weapons changed hands since the state was
  // saved (a pick-up the prediction did not know of).
  std::vector<weapon::WeaponItem> weapons;
  weapons.reserve(weapon::kWeaponSlotCount);
  for (size_t i = 0; i < state.slot_keys.size(); ++i) {
    if (!state.loadout.slots[i].present) {
      continue;
    }
    const std::optional<weapon::WeaponDefId> def =
        table_.Find(state.slot_keys[i]);
    if (!def) {
      return false;
    }
    weapons.push_back(weapon::MakeFreshWeapon(*def, library_.def(*def)));
  }
  loadout_ = weapon::Loadout(weapons, loadout_.tuning());
  zero_def_.reset();
  return true;
}

float PlayerSim::ZeroDistance(const weapon::WeaponItem& item) const {
  const weapon::WeaponDef& def = library_.def(item.def);
  if (def.type.scope) {
    return weapon::ClampZero(*def.type.scope, item.scope_zero);
  }
  if (def.zero_range) {
    return weapon::parts::ClampZero(*def.zero_range, item.scope_zero);
  }
  return def.type.viewmodel.zero_distance;
}

float PlayerSim::ZeroAngle(const weapon::WeaponItem& item) {
  const float distance = ZeroDistance(item);
  if (zero_def_ != item.def || zero_distance_ != distance) {
    // The viewmodel holds the eye-to-bore geometry the game zeroes with.
    const weapon::WeaponDef& def = library_.def(item.def);
    weapon::Viewmodel viewmodel(def.geometry, def.type.viewmodel);
    viewmodel.SetZeroDistance(distance);
    zero_def_ = item.def;
    zero_distance_ = distance;
    zero_angle_ = viewmodel.zero_angle();
  }
  return zero_angle_;
}

void PlayerSim::AddShot(float age, StepEvents& events) {
  const weapon::WeaponItem& item = *loadout_.active_item();
  const weapon::WeaponDef& def = library_.def(item.def);
  const weapon::FirearmSpec& spec = item.firearm.spec();
  const camera::LookAngles zeroed{.yaw = look_.yaw,
                                  .pitch = look_.pitch + ZeroAngle(item)};
  ++shots_fired_;
  weapon::Dispersion dispersion(ShotSeed(id_, shots_fired_));
  const glm::vec3 direction =
      dispersion.Apply(camera::ForwardFrom(zeroed), def.accuracy_moa);
  const glm::vec3 shooter_velocity = character_->velocity();
  ShotEvent& shot = events.shots[static_cast<size_t>(events.shot_count++)];
  shot = {.origin = eye(),
          .direction = direction,
          .muzzle_velocity = spec.muzzle_velocity,
          .shooter_velocity = shooter_velocity,
          .velocity = direction * spec.muzzle_velocity + shooter_velocity,
          .mass = spec.bullet_mass,
          .drag = spec.bullet_drag,
          .age = age,
          .shot_number = shots_fired_,
          .weapon_key = table_.KeyOf(item.def).value_or(0)};
}

void PlayerSim::ApplyZeroKeys(const UserCommand& command) {
  const int clicks =
      (Has(command.pressed, buttons::pressed::kZeroUp) ? 1 : 0) -
      (Has(command.pressed, buttons::pressed::kZeroDown) ? 1 : 0);
  const weapon::WeaponItem* item = loadout_.active_item();
  if (clicks == 0 || item == nullptr || loadout_.switching()) {
    return;
  }
  const weapon::WeaponDef& def = library_.def(item->def);
  const float current = ZeroDistance(*item);
  if (def.type.scope) {
    (void)loadout_.SetScopeZero(
        weapon::StepZero(*def.type.scope, current, clicks));
  } else if (def.zero_range) {
    (void)loadout_.SetScopeZero(
        weapon::parts::StepZero(*def.zero_range, current, clicks));
  }
}

}  // namespace blue::sim
