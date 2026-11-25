#include "sim/c_api/blue_sim.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include "core/log.h"
#include "physics/physics_runtime.h"
#include "physics/physics_world.h"
#include "scene/scene_data.h"
#include "sim/damage.h"
#include "sim/player_state.h"
#include "sim/sim_world.h"
#include "sim/user_command.h"
#include "weapon/firearm.h"
#include "weapon/loadout.h"
#include "weapon/projectile_system.h"

// The C ABI's handle: a SimWorld plus the last error and the buffers the
// read-outs convert through (kept, so read-outs stop allocating once grown).
// A C++ type with the C name of the opaque handle (documented C interop
// exception to the naming rules).
// Scratch members are mutable: the const read-outs convert through them.
struct BlueSim {
  std::unique_ptr<blue::sim::SimWorld> world;
  mutable std::string last_error;
  mutable std::vector<blue::sim::PlayerReadout> players;
  mutable std::vector<blue::sim::BodyReadout> bodies;
  mutable std::vector<blue::sim::DroppedReadout> dropped;
  mutable std::vector<blue::sim::PickupReadout> pickups;
  mutable std::vector<blue::sim::SimEvent> events;
  mutable std::vector<blue::sim::ShotHit> hits;
};

namespace blue::sim {
namespace {

// --- Layout checks: the C structs have exactly these sizes and offsets on
// every supported platform (Rust's repr(C) mirrors check the sizes through
// blue_sim_struct_size()).
static_assert(sizeof(BlueVec3) == 12);
static_assert(sizeof(BlueQuat) == 16);
static_assert(sizeof(BlueUserCommand) == 28);
static_assert(offsetof(BlueUserCommand, select_slot) == 20);
static_assert(offsetof(BlueUserCommand, interact_kind) == 21);
static_assert(offsetof(BlueUserCommand, interact_id) == 24);
static_assert(sizeof(BlueFirearmState) == 44);
static_assert(offsetof(BlueFirearmState, magazine) == 8);
static_assert(offsetof(BlueFirearmState, incoming_magazine) == 10);
static_assert(offsetof(BlueFirearmState, spares) == 12);
static_assert(offsetof(BlueFirearmState, spares_dropped) == 28);
static_assert(offsetof(BlueFirearmState, reload_elapsed) == 32);
static_assert(offsetof(BlueFirearmState, cycle_remaining) == 40);
static_assert(sizeof(BlueLoadoutSlot) == 60);
static_assert(offsetof(BlueLoadoutSlot, weapon_key) == 4);
static_assert(offsetof(BlueLoadoutSlot, firearm) == 8);
static_assert(offsetof(BlueLoadoutSlot, scope_magnification) == 52);
static_assert(offsetof(BlueLoadoutSlot, scope_zero) == 56);
static_assert(sizeof(BluePlayerState) == 200);
static_assert(offsetof(BluePlayerState, position) == 4);
static_assert(offsetof(BluePlayerState, velocity) == 16);
static_assert(offsetof(BluePlayerState, jump_speed) == 28);
static_assert(offsetof(BluePlayerState, jump_age) == 32);
static_assert(offsetof(BluePlayerState, jump_cooldown) == 36);
static_assert(offsetof(BluePlayerState, yaw) == 40);
static_assert(offsetof(BluePlayerState, pitch) == 44);
static_assert(offsetof(BluePlayerState, eye_height) == 48);
static_assert(offsetof(BluePlayerState, health) == 52);
static_assert(offsetof(BluePlayerState, respawn_ticks) == 54);
static_assert(offsetof(BluePlayerState, shots_fired) == 56);
static_assert(offsetof(BluePlayerState, deaths) == 60);
static_assert(offsetof(BluePlayerState, jump_pending) == 64);
static_assert(offsetof(BluePlayerState, active_slot) == 70);
static_assert(offsetof(BluePlayerState, selected_slot) == 71);
static_assert(offsetof(BluePlayerState, raise) == 72);
static_assert(offsetof(BluePlayerState, wait_for_release) == 76);
static_assert(offsetof(BluePlayerState, slots) == 80);
static_assert(sizeof(BlueSegment) == 72);
static_assert(offsetof(BlueSegment, origin) == 16);
static_assert(offsetof(BlueSegment, delta) == 28);
static_assert(offsetof(BlueSegment, velocity) == 40);
static_assert(offsetof(BlueSegment, mass) == 52);
static_assert(offsetof(BlueSegment, world_normal) == 56);
static_assert(offsetof(BlueSegment, world_instance) == 68);
static_assert(sizeof(BlueShotHit) == 16);
static_assert(sizeof(BlueCapsule) == 20);
static_assert(sizeof(BlueRayHit) == 36);
static_assert(offsetof(BlueRayHit, fraction) == 24);
static_assert(sizeof(BlueBodyState) == 60);
static_assert(offsetof(BlueBodyState, position) == 8);
static_assert(offsetof(BlueBodyState, rotation) == 20);
static_assert(offsetof(BlueBodyState, linear_velocity) == 36);
static_assert(offsetof(BlueBodyState, angular_velocity) == 48);
static_assert(sizeof(BlueDroppedWeapon) == 76);
static_assert(offsetof(BlueDroppedWeapon, position) == 8);
static_assert(offsetof(BlueDroppedWeapon, rotation) == 20);
static_assert(offsetof(BlueDroppedWeapon, scale) == 36);
static_assert(offsetof(BlueDroppedWeapon, linear_velocity) == 48);
static_assert(offsetof(BlueDroppedWeapon, angular_velocity) == 60);
static_assert(offsetof(BlueDroppedWeapon, magazine) == 72);
static_assert(offsetof(BlueDroppedWeapon, chambered) == 74);
static_assert(offsetof(BlueDroppedWeapon, active) == 75);
static_assert(sizeof(BluePickup) == 40);
static_assert(offsetof(BluePickup, position) == 12);
static_assert(offsetof(BluePickup, rotation) == 24);
static_assert(sizeof(BlueSimEvent) == 60);
static_assert(offsetof(BlueSimEvent, chambered) == 2);
static_assert(offsetof(BlueSimEvent, tick) == 4);
static_assert(offsetof(BlueSimEvent, weapon_key) == 20);
static_assert(offsetof(BlueSimEvent, value) == 24);
static_assert(offsetof(BlueSimEvent, health) == 28);
static_assert(offsetof(BlueSimEvent, yaw) == 32);
static_assert(offsetof(BlueSimEvent, position) == 36);
static_assert(offsetof(BlueSimEvent, vector) == 48);
// The C constants mirror the C++ enumerations.
static_assert(BLUE_SIM_EVENT_PICKUP_TAKEN ==
              static_cast<int>(SimEventType::kPickupTaken));
static_assert(BLUE_SIM_EVENT_DROPPED_REMOVED ==
              static_cast<int>(SimEventType::kDroppedRemoved));
static_assert(BLUE_SIM_LOG_INFO == static_cast<int>(core::log::Level::kInfo));
static_assert(BLUE_SIM_LOG_WARN == static_cast<int>(core::log::Level::kWarn));
static_assert(BLUE_SIM_LOG_ERROR == static_cast<int>(core::log::Level::kError));
static_assert(BLUE_SIM_LOG_FATAL == static_cast<int>(core::log::Level::kFatal));
static_assert(BLUE_SIM_ZONE_LEGS == static_cast<int>(HitZone::kLegs));
static_assert(BLUE_SIM_INTERACT_SCENE_WEAPON == interact_kind::kSceneWeapon);
static_assert(BLUE_SIM_INTERACT_DROPPED_WEAPON ==
              interact_kind::kDroppedWeapon);
static_assert(weapon::kMaxSavedSpareMagazines == 8);
static_assert(weapon::kWeaponSlotCount == 2);

// The process's physics runtime, shared by every BlueSim: created with the
// first and destroyed with the last (PhysicsRuntime allows one at a time).
// Trivially destructible globals, touched from the creating thread only.
constinit physics::PhysicsRuntime* g_runtime = nullptr;
constinit int g_runtime_users = 0;

// The C callback ForwardLog() calls; set before the core::log sink.
constinit BlueSimLogCallback g_log_callback = nullptr;

// core::log sink: hands each line, NUL-terminated, to g_log_callback.
// core::log never calls a sink on two threads at once, so the buffer may be
// shared; it is leaked so logging during static destruction stays safe.
void ForwardLog(core::log::Level level, std::string_view message, void* user) {
  static std::string& buffer = *new std::string;
  buffer.assign(message);
  g_log_callback(user, static_cast<int32_t>(level), buffer.c_str());
}

// Copies `text` into `buffer` (cap bytes, NUL-terminated, truncated).
void CopyText(std::string_view text, char* buffer, int32_t cap) {
  if (buffer == nullptr || cap <= 0) {
    return;
  }
  const size_t count = std::min(text.size(), static_cast<size_t>(cap) - 1);
  std::memcpy(buffer, text.data(), count);
  buffer[count] = '\0';
}

int32_t Fail(const BlueSim* sim, int32_t status, std::string message) {
  if (sim != nullptr) {
    sim->last_error = std::move(message);
  }
  return status;
}

// Returns `count` as an int32_t status-or-count, saturated.
int32_t CountOf(size_t count) {
  return static_cast<int32_t>(
      std::min<size_t>(count, std::numeric_limits<int32_t>::max()));
}

// Checks a caller's output buffer for `needed` entries.
int32_t CheckBuffer(const BlueSim* sim, const void* out, int32_t cap,
                    size_t needed) {
  if (cap < 0 || (out == nullptr && cap > 0)) {
    return Fail(sim, BLUE_SIM_ERR_INVALID_ARGUMENT,
                "negative capacity or null buffer with a capacity");
  }
  if (static_cast<size_t>(cap) < needed) {
    return Fail(sim, BLUE_SIM_ERR_BUFFER_TOO_SMALL,
                "buffer of " + std::to_string(cap) + " entries, " +
                    std::to_string(needed) + " needed");
  }
  return BLUE_SIM_OK;
}

BlueVec3 ToC(glm::vec3 v) { return {.x = v.x, .y = v.y, .z = v.z}; }
glm::vec3 FromC(const BlueVec3& v) { return {v.x, v.y, v.z}; }
BlueQuat ToC(const glm::quat& q) {
  return {.x = q.x, .y = q.y, .z = q.z, .w = q.w};
}
uint8_t Flag(bool value) { return value ? 1 : 0; }
int16_t Int16Of(int value) {
  return static_cast<int16_t>(
      std::clamp<int>(value, std::numeric_limits<int16_t>::min(),
                      std::numeric_limits<int16_t>::max()));
}
int8_t SlotIndex(std::optional<weapon::WeaponSlot> slot) {
  return slot ? static_cast<int8_t>(*slot) : int8_t{-1};
}

UserCommand FromC(const BlueUserCommand& c) {
  UserCommand command;
  command.tick = c.tick;
  command.held = c.held;
  command.pressed = c.pressed;
  command.yaw = c.yaw;
  command.pitch = c.pitch;
  command.select_slot = c.select_slot;
  command.interact_kind = c.interact_kind;
  command.interact_id = c.interact_id;
  return command;
}

BlueFirearmState ToC(const weapon::FirearmState& f) {
  BlueFirearmState c{};
  c.fire_mode = static_cast<uint8_t>(f.fire_mode);
  c.magazine_inserted = Flag(f.magazine_inserted);
  c.chambered = Flag(f.chambered);
  c.bolt_locked = Flag(f.bolt_locked);
  c.empty_reload = Flag(f.empty_reload);
  c.pull_used = Flag(f.pull_used);
  c.reload_stage = f.reload_stage;
  c.spare_count = f.spare_count;
  c.magazine = f.magazine;
  c.incoming_magazine = f.incoming_magazine;
  for (size_t i = 0; i < f.spares.size(); ++i) {
    c.spares[i] = f.spares[i];
  }
  c.spares_dropped = f.spares_dropped;
  c.reload_elapsed = f.reload_elapsed;
  c.reload_duration = f.reload_duration;
  c.cycle_remaining = f.cycle_remaining;
  return c;
}

BluePlayerState ToC(const PlayerReadout& readout) {
  const PlayerState& s = readout.state;
  BluePlayerState c{};
  c.id = readout.id;
  c.position = ToC(s.position);
  c.velocity = ToC(s.velocity);
  c.jump_speed = s.jump_speed;
  c.jump_age = s.jump_age;
  c.jump_cooldown = s.jump_cooldown;
  c.yaw = s.yaw;
  c.pitch = s.pitch;
  c.eye_height = s.eye_height;
  c.health = s.health;
  c.respawn_ticks = s.respawn_ticks;
  c.shots_fired = s.shots_fired;
  c.deaths = s.deaths;
  c.jump_pending = Flag(s.jump_pending);
  c.crouching = Flag(s.crouching);
  c.on_ground = Flag(s.on_ground);
  c.sprinting = Flag(s.sprinting);
  c.aiming = Flag(s.aiming);
  c.alive = Flag(s.alive);
  c.active_slot = SlotIndex(s.loadout.active);
  c.selected_slot = SlotIndex(s.loadout.selected);
  c.raise = s.loadout.raise;
  c.wait_for_release = Flag(s.loadout.wait_for_release);
  for (size_t i = 0; i < s.loadout.slots.size(); ++i) {
    const weapon::LoadoutSlotState& slot = s.loadout.slots[i];
    BlueLoadoutSlot& out = c.slots[i];
    out.present = Flag(slot.present);
    out.weapon_key = s.slot_keys[i];
    out.firearm = ToC(slot.firearm);
    out.scope_magnification = slot.scope_magnification;
    out.scope_zero = slot.scope_zero;
  }
  return c;
}

BlueSimEvent ToC(const SimEvent& e) {
  BlueSimEvent c{};
  c.type = static_cast<uint8_t>(e.type);
  c.zone = static_cast<uint8_t>(e.zone);
  c.chambered = Flag(e.chambered);
  c.tick = e.tick;
  c.player = e.player;
  c.other = e.other;
  c.id = e.id;
  c.weapon_key = e.weapon_key;
  c.value = e.value;
  c.health = e.health;
  c.yaw = e.yaw;
  c.position = ToC(e.position);
  c.vector = ToC(e.vector);
  return c;
}

// Copies the read-out `items` (count `total`) of `sim` to `out` through
// `convert`.
// A null `out` with cap 0 only asks for the count.
template <typename In, typename Out, typename Convert>
int32_t CopyOut(const BlueSim* sim, std::span<const In> items, size_t total,
                Out* out, int32_t cap, Convert convert) {
  if (out == nullptr && cap == 0) {
    return CountOf(total);
  }
  if (const int32_t status = CheckBuffer(sim, out, cap, total);
      status != BLUE_SIM_OK) {
    return status;
  }
  for (size_t i = 0; i < total; ++i) {
    out[i] = convert(items[i]);
  }
  return CountOf(total);
}

// Reads a SimWorld read-out into `buffer`, growing it to the total.
template <typename T, typename Read>
size_t ReadAll(std::vector<T>& buffer, Read read) {
  size_t total = read(std::span<T>(buffer));
  if (total > buffer.size()) {
    buffer.resize(total);
    total = read(std::span<T>(buffer));
  }
  return total;
}

}  // namespace
}  // namespace blue::sim

namespace sim = blue::sim;

extern "C" {

int32_t blue_sim_abi_version(void) noexcept { return BLUE_SIM_ABI_VERSION; }

int32_t blue_sim_struct_size(int32_t struct_id) noexcept {
  switch (struct_id) {
    case BLUE_SIM_STRUCT_CONFIG:
      return sizeof(BlueSimConfig);
    case BLUE_SIM_STRUCT_VEC3:
      return sizeof(BlueVec3);
    case BLUE_SIM_STRUCT_QUAT:
      return sizeof(BlueQuat);
    case BLUE_SIM_STRUCT_USER_COMMAND:
      return sizeof(BlueUserCommand);
    case BLUE_SIM_STRUCT_FIREARM_STATE:
      return sizeof(BlueFirearmState);
    case BLUE_SIM_STRUCT_LOADOUT_SLOT:
      return sizeof(BlueLoadoutSlot);
    case BLUE_SIM_STRUCT_PLAYER_STATE:
      return sizeof(BluePlayerState);
    case BLUE_SIM_STRUCT_SEGMENT:
      return sizeof(BlueSegment);
    case BLUE_SIM_STRUCT_SHOT_HIT:
      return sizeof(BlueShotHit);
    case BLUE_SIM_STRUCT_CAPSULE:
      return sizeof(BlueCapsule);
    case BLUE_SIM_STRUCT_RAY_HIT:
      return sizeof(BlueRayHit);
    case BLUE_SIM_STRUCT_BODY_STATE:
      return sizeof(BlueBodyState);
    case BLUE_SIM_STRUCT_DROPPED_WEAPON:
      return sizeof(BlueDroppedWeapon);
    case BLUE_SIM_STRUCT_PICKUP:
      return sizeof(BluePickup);
    case BLUE_SIM_STRUCT_EVENT:
      return sizeof(BlueSimEvent);
    default:
      return BLUE_SIM_ERR_INVALID_ARGUMENT;
  }
}

int32_t blue_sim_create(const BlueSimConfig* config, BlueSim** out, char* err,
                        int32_t err_cap) noexcept {
  sim::CopyText("", err, err_cap);
  if (out == nullptr) {
    sim::CopyText("out is NULL", err, err_cap);
    return BLUE_SIM_ERR_NULL;
  }
  *out = nullptr;
  if (config == nullptr) {
    sim::CopyText("config is NULL", err, err_cap);
    return BLUE_SIM_ERR_NULL;
  }
  if (config->asset_dir == nullptr || config->scene_path == nullptr) {
    sim::CopyText("config->asset_dir or config->scene_path is NULL", err,
                  err_cap);
    return BLUE_SIM_ERR_NULL;
  }
  if (config->worker_threads < 0 || config->max_players <= 0) {
    sim::CopyText("worker_threads must be >= 0 and max_players > 0", err,
                  err_cap);
    return BLUE_SIM_ERR_INVALID_ARGUMENT;
  }
  if (sim::g_runtime == nullptr) {
    sim::g_runtime = new blue::physics::PhysicsRuntime();
  }
  ++sim::g_runtime_users;
  auto created = sim::SimWorld::Create(
      *sim::g_runtime, {.asset_dir = config->asset_dir,
                        .scene_path = config->scene_path,
                        .worker_threads = config->worker_threads,
                        .max_players = config->max_players});
  if (!created.ok()) {
    if (--sim::g_runtime_users == 0) {
      delete sim::g_runtime;
      sim::g_runtime = nullptr;
    }
    blue::core::log::Error("blue_sim_create: {}", created.error());
    sim::CopyText(created.error(), err, err_cap);
    return BLUE_SIM_ERR_CREATE_FAILED;
  }
  *out = new BlueSim{.world = std::move(created).value()};
  return BLUE_SIM_OK;
}

int32_t blue_sim_destroy(BlueSim* sim) noexcept {
  if (sim == nullptr) {
    return BLUE_SIM_ERR_NULL;
  }
  delete sim;  // The world (and its bodies) before the runtime.
  if (--sim::g_runtime_users == 0) {
    delete sim::g_runtime;
    sim::g_runtime = nullptr;
  }
  return BLUE_SIM_OK;
}

int32_t blue_sim_last_error(const BlueSim* sim, char* buffer,
                            int32_t cap) noexcept {
  if (sim == nullptr) {
    return BLUE_SIM_ERR_NULL;
  }
  if (cap < 0 || (buffer == nullptr && cap > 0)) {
    return BLUE_SIM_ERR_INVALID_ARGUMENT;
  }
  sim::CopyText(sim->last_error, buffer, cap);
  return sim::CountOf(sim->last_error.size());
}

int32_t blue_sim_set_log_callback(BlueSimLogCallback callback,
                                  void* user) noexcept {
  sim::g_log_callback = callback;
  blue::core::log::SetSink(callback != nullptr ? sim::ForwardLog : nullptr,
                           user);
  return BLUE_SIM_OK;
}

int32_t blue_sim_content_hash(const BlueSim* sim, uint64_t* out) noexcept {
  if (sim == nullptr || out == nullptr) {
    return BLUE_SIM_ERR_NULL;
  }
  *out = sim->world->content_hash();
  return BLUE_SIM_OK;
}

int32_t blue_sim_add_player(BlueSim* sim, uint32_t id) noexcept {
  if (sim == nullptr) {
    return BLUE_SIM_ERR_NULL;
  }
  if (!sim->world->AddPlayer(id)) {
    return sim::Fail(sim, BLUE_SIM_ERR_PLAYER_REFUSED,
                     "player " + std::to_string(id) +
                         " refused: id in use, sim full or no body");
  }
  return BLUE_SIM_OK;
}

int32_t blue_sim_remove_player(BlueSim* sim, uint32_t id) noexcept {
  if (sim == nullptr) {
    return BLUE_SIM_ERR_NULL;
  }
  if (!sim->world->RemovePlayer(id)) {
    return sim::Fail(sim, BLUE_SIM_ERR_UNKNOWN_PLAYER,
                     "unknown player " + std::to_string(id));
  }
  return BLUE_SIM_OK;
}

int32_t blue_sim_apply_command(BlueSim* sim, uint32_t id,
                               const BlueUserCommand* command) noexcept {
  if (sim == nullptr) {
    return BLUE_SIM_ERR_NULL;
  }
  if (command == nullptr) {
    return sim::Fail(sim, BLUE_SIM_ERR_NULL, "command is NULL");
  }
  if (!sim->world->ApplyCommand(id, sim::FromC(*command))) {
    return sim::Fail(sim, BLUE_SIM_ERR_UNKNOWN_PLAYER,
                     "unknown player " + std::to_string(id));
  }
  return BLUE_SIM_OK;
}

int32_t blue_sim_step(BlueSim* sim, uint32_t tick) noexcept {
  if (sim == nullptr) {
    return BLUE_SIM_ERR_NULL;
  }
  sim->world->Step(tick);
  return BLUE_SIM_OK;
}

int32_t blue_sim_projectiles_sweep(BlueSim* sim, BlueSegment* out,
                                   int32_t cap) noexcept {
  if (sim == nullptr) {
    return BLUE_SIM_ERR_NULL;
  }
  const std::span<const blue::weapon::Segment> segments =
      sim->world->SweepProjectiles();
  const sim::SimWorld& world = *sim->world;
  return sim::CopyOut(sim, segments, segments.size(), out, cap,
                      [&world](const blue::weapon::Segment& s) {
                        BlueSegment c{};
                        c.projectile = s.projectile;
                        c.shot_number = s.shot_number;
                        c.owner = s.owner;
                        c.world_fraction = s.world_fraction;
                        c.origin = sim::ToC(s.origin);
                        c.delta = sim::ToC(s.delta);
                        c.velocity = sim::ToC(s.velocity);
                        c.mass = s.mass;
                        c.world_normal = sim::ToC(s.world_normal);
                        if (s.world_fraction < 1.0f) {
                          c.world_instance = world.InstanceOf(s.world_body);
                        }
                        return c;
                      });
}

int32_t blue_sim_projectiles_resolve(BlueSim* sim, const BlueShotHit* hits,
                                     int32_t count) noexcept {
  if (sim == nullptr) {
    return BLUE_SIM_ERR_NULL;
  }
  if (count < 0 || (hits == nullptr && count > 0)) {
    return sim::Fail(sim, BLUE_SIM_ERR_INVALID_ARGUMENT,
                     "negative hit count or null hits with a count");
  }
  sim->hits.clear();
  for (int32_t i = 0; i < count; ++i) {
    const BlueShotHit& hit = hits[i];
    sim->hits.push_back({.segment = hit.segment,
                         .victim = hit.victim,
                         .fraction = hit.fraction,
                         .height_fraction = hit.height_fraction});
  }
  sim->world->ResolveProjectiles(sim->hits);
  return BLUE_SIM_OK;
}

int32_t blue_sim_hit_capsules_now(const BlueSim* sim, BlueShotHit* out,
                                  int32_t cap) noexcept {
  if (sim == nullptr) {
    return BLUE_SIM_ERR_NULL;
  }
  sim->world->HitCapsulesNow(sim->hits);
  return sim::CopyOut(sim, std::span<const sim::ShotHit>(sim->hits),
                      sim->hits.size(), out, cap, [](const sim::ShotHit& hit) {
                        return BlueShotHit{
                            .segment = hit.segment,
                            .victim = hit.victim,
                            .fraction = hit.fraction,
                            .height_fraction = hit.height_fraction};
                      });
}

int32_t blue_sim_player_capsule(const BlueSim* sim, uint32_t id,
                                BlueCapsule* out) noexcept {
  if (sim == nullptr) {
    return BLUE_SIM_ERR_NULL;
  }
  if (out == nullptr) {
    return sim::Fail(sim, BLUE_SIM_ERR_NULL, "out is NULL");
  }
  const std::optional<sim::Capsule> capsule = sim->world->CurrentCapsule(id);
  if (!capsule) {
    return sim::Fail(sim, BLUE_SIM_ERR_UNKNOWN_PLAYER,
                     "unknown player " + std::to_string(id));
  }
  *out = {.feet = sim::ToC(capsule->feet),
          .height = capsule->height,
          .radius = capsule->radius};
  return BLUE_SIM_OK;
}

int32_t blue_sim_cast_ray(const BlueSim* sim, const BlueVec3* origin,
                          const BlueVec3* direction, float max_distance,
                          BlueRayHit* out) noexcept {
  if (sim == nullptr) {
    return BLUE_SIM_ERR_NULL;
  }
  if (origin == nullptr || direction == nullptr || out == nullptr) {
    return sim::Fail(sim, BLUE_SIM_ERR_NULL,
                     "origin, direction or out is NULL");
  }
  const std::optional<sim::SimRayHit> hit = sim->world->CastRay(
      sim::FromC(*origin), sim::FromC(*direction), max_distance);
  *out = {};
  if (!hit) {
    return 0;
  }
  *out = {.point = sim::ToC(hit->point),
          .normal = sim::ToC(hit->normal),
          .fraction = hit->fraction,
          .instance = hit->instance,
          .dropped_id = hit->dropped_id};
  return 1;
}

int32_t blue_sim_read_players(const BlueSim* sim, BluePlayerState* out,
                              int32_t cap) noexcept {
  if (sim == nullptr) {
    return BLUE_SIM_ERR_NULL;
  }
  const BlueSim& scratch = *sim;
  const size_t total = sim::ReadAll(
      scratch.players, [sim](std::span<sim::PlayerReadout> buffer) {
        return sim->world->ReadPlayers(buffer);
      });
  return sim::CopyOut(sim, std::span<const sim::PlayerReadout>(scratch.players),
                      total, out, cap,
                      [](const sim::PlayerReadout& p) { return sim::ToC(p); });
}

int32_t blue_sim_read_bodies(const BlueSim* sim, BlueBodyState* out,
                             int32_t cap) noexcept {
  if (sim == nullptr) {
    return BLUE_SIM_ERR_NULL;
  }
  const BlueSim& scratch = *sim;
  const size_t total =
      sim::ReadAll(scratch.bodies, [sim](std::span<sim::BodyReadout> buffer) {
        return sim->world->ReadBodies(buffer);
      });
  return sim::CopyOut(sim, std::span<const sim::BodyReadout>(scratch.bodies),
                      total, out, cap, [](const sim::BodyReadout& b) {
                        BlueBodyState c{};
                        c.instance = b.instance;
                        c.active = sim::Flag(b.state.active);
                        c.position = sim::ToC(b.state.pose.position);
                        c.rotation = sim::ToC(b.state.pose.rotation);
                        c.linear_velocity = sim::ToC(b.state.linear_velocity);
                        c.angular_velocity = sim::ToC(b.state.angular_velocity);
                        return c;
                      });
}

int32_t blue_sim_read_dropped_weapons(const BlueSim* sim,
                                      BlueDroppedWeapon* out,
                                      int32_t cap) noexcept {
  if (sim == nullptr) {
    return BLUE_SIM_ERR_NULL;
  }
  const BlueSim& scratch = *sim;
  const size_t total = sim::ReadAll(
      scratch.dropped, [sim](std::span<sim::DroppedReadout> buffer) {
        return sim->world->ReadDroppedWeapons(buffer);
      });
  return sim::CopyOut(sim,
                      std::span<const sim::DroppedReadout>(scratch.dropped),
                      total, out, cap, [](const sim::DroppedReadout& d) {
                        return BlueDroppedWeapon{
                            .id = d.id,
                            .weapon_key = d.weapon_key,
                            .position = sim::ToC(d.pose.position),
                            .rotation = sim::ToC(d.pose.rotation),
                            .scale = sim::ToC(d.scale),
                            .linear_velocity = sim::ToC(d.linear_velocity),
                            .angular_velocity = sim::ToC(d.angular_velocity),
                            .magazine = sim::Int16Of(d.magazine),
                            .chambered = sim::Flag(d.chambered),
                            .active = sim::Flag(d.active)};
                      });
}

int32_t blue_sim_read_pickups(const BlueSim* sim, BluePickup* out,
                              int32_t cap) noexcept {
  if (sim == nullptr) {
    return BLUE_SIM_ERR_NULL;
  }
  const BlueSim& scratch = *sim;
  const size_t total = sim::ReadAll(
      scratch.pickups, [sim](std::span<sim::PickupReadout> buffer) {
        return sim->world->ReadPickups(buffer);
      });
  return sim::CopyOut(sim, std::span<const sim::PickupReadout>(scratch.pickups),
                      total, out, cap, [](const sim::PickupReadout& p) {
                        BluePickup c{};
                        c.instance = p.instance;
                        c.weapon_key = p.weapon_key;
                        c.taken = sim::Flag(p.taken);
                        c.position = sim::ToC(p.pose.position);
                        c.rotation = sim::ToC(p.pose.rotation);
                        return c;
                      });
}

int32_t blue_sim_poll_events(BlueSim* sim, BlueSimEvent* out,
                             int32_t cap) noexcept {
  if (sim == nullptr) {
    return BLUE_SIM_ERR_NULL;
  }
  if (cap < 0 || (out == nullptr && cap > 0)) {
    return sim::Fail(sim, BLUE_SIM_ERR_INVALID_ARGUMENT,
                     "negative capacity or null buffer with a capacity");
  }
  sim->events.resize(
      std::min(static_cast<size_t>(cap), sim->world->pending_events()));
  const size_t count = sim->world->PollEvents(sim->events);
  for (size_t i = 0; i < count; ++i) {
    out[i] = sim::ToC(sim->events[i]);
  }
  return sim::CountOf(count);
}

}  // extern "C"
