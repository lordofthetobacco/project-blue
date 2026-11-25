/* C ABI over blue_sim (sim/sim_world.h), used by the Rust dedicated server
 * (crates/blue-native). Plain C11: no C++ types cross this boundary.
 *
 * Naming: snake_case functions (blue_sim_*) and Blue* types, a documented
 * exception to the project's C++ naming rules for C interop.
 *
 * Structs hold only fixed-width integers, floats and fixed arrays, with
 * explicit _pad fields so their layout has no implicit padding; the only
 * pointers are in BlueSimConfig. Their sizes are checked at compile time in
 * blue_sim.cc and reported by blue_sim_struct_size() so a foreign mirror can
 * verify itself at startup. Pad fields are written as 0 and ignored on input.
 *
 * Every function is null-safe and never aborts: it returns a status
 * (BLUE_SIM_OK or a negative BLUE_SIM_ERR_*) or a count (>= 0). After a
 * failure on a valid handle, blue_sim_last_error() describes it.
 *
 * A BlueSim is not thread-safe: call it from one thread. Creating and
 * destroying sims must happen on one thread too (they share the process's
 * physics runtime, created with the first sim and destroyed with the last).
 * Coordinates are SI units, right-handed, Y up; angles in radians.
 *
 * A server tick:
 *   blue_sim_apply_command() for each command of the tick;
 *   blue_sim_step(tick);
 *   blue_sim_projectiles_sweep() -> segments; test them against the
 *     lag-compensated capsules; blue_sim_projectiles_resolve(hits);
 *   blue_sim_read_*() and blue_sim_poll_events() for the snapshot. */
#ifndef BLUE_SIM_C_API_BLUE_SIM_H_
#define BLUE_SIM_C_API_BLUE_SIM_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Marks the functions noexcept for C++ callers (they never throw). */
#ifdef __cplusplus
#define BLUE_SIM_NOEXCEPT noexcept
#else
#define BLUE_SIM_NOEXCEPT
#endif

/* Bumped on every incompatible change of a struct or function.
 * 3: BLUE_SIM_EVENT_DROPPED_REMOVED (dropped weapons despawn: lifetime,
 *    cap, kill plane); players below the kill plane die (DEATH, other 0).
 * 2: BlueDroppedWeapon gained velocities, rounds and active; BlueSimEvent
 *    gained chambered and yaw; damage and death events carry weapon_key;
 *    BLUE_SIM_EVENT_HIT_CONFIRMED retired (the damage event is the hit
 *    confirmation); the log callback receives every engine log line. */
#define BLUE_SIM_ABI_VERSION 3

/* Status codes. */
#define BLUE_SIM_OK 0
#define BLUE_SIM_ERR_NULL (-1)             /* null handle or pointer */
#define BLUE_SIM_ERR_INVALID_ARGUMENT (-2) /* bad value; see last_error */
#define BLUE_SIM_ERR_CREATE_FAILED (-3)    /* blue_sim_create; see err */
#define BLUE_SIM_ERR_BUFFER_TOO_SMALL (-4) /* nothing written; call again */
#define BLUE_SIM_ERR_UNKNOWN_PLAYER (-5)
#define BLUE_SIM_ERR_PLAYER_REFUSED (-6) /* id in use, full, or no body */

/* blue_sim_struct_size() ids. */
#define BLUE_SIM_STRUCT_CONFIG 1
#define BLUE_SIM_STRUCT_VEC3 2
#define BLUE_SIM_STRUCT_QUAT 3
#define BLUE_SIM_STRUCT_USER_COMMAND 4
#define BLUE_SIM_STRUCT_FIREARM_STATE 5
#define BLUE_SIM_STRUCT_LOADOUT_SLOT 6
#define BLUE_SIM_STRUCT_PLAYER_STATE 7
#define BLUE_SIM_STRUCT_SEGMENT 8
#define BLUE_SIM_STRUCT_SHOT_HIT 9
#define BLUE_SIM_STRUCT_CAPSULE 10
#define BLUE_SIM_STRUCT_RAY_HIT 11
#define BLUE_SIM_STRUCT_BODY_STATE 12
#define BLUE_SIM_STRUCT_DROPPED_WEAPON 13
#define BLUE_SIM_STRUCT_PICKUP 14
#define BLUE_SIM_STRUCT_EVENT 15

/* BlueSimEvent::type (sim::SimEventType). */
#define BLUE_SIM_EVENT_SHOT 0
#define BLUE_SIM_EVENT_IMPACT 1
#define BLUE_SIM_EVENT_DAMAGE 2
#define BLUE_SIM_EVENT_DEATH 3
#define BLUE_SIM_EVENT_RESPAWN 4
#define BLUE_SIM_EVENT_WEAPON_DROPPED 5
#define BLUE_SIM_EVENT_DROPPED_TAKEN 6
#define BLUE_SIM_EVENT_PICKUP_TAKEN 7
/* 8 was BLUE_SIM_EVENT_HIT_CONFIRMED (ABI 1); never emitted. */
#define BLUE_SIM_EVENT_DROPPED_REMOVED 9

/* BlueSimEvent::zone (sim::HitZone). */
#define BLUE_SIM_ZONE_HEAD 0
#define BLUE_SIM_ZONE_TORSO 1
#define BLUE_SIM_ZONE_LEGS 2

/* BlueUserCommand::interact_kind (sim::interact_kind). */
#define BLUE_SIM_INTERACT_NONE 0
#define BLUE_SIM_INTERACT_SCENE_WEAPON 1   /* interact_id: scene instance */
#define BLUE_SIM_INTERACT_DROPPED_WEAPON 2 /* interact_id: dropped item id */

/* blue_sim_set_log_callback() levels. */
#define BLUE_SIM_LOG_INFO 0
#define BLUE_SIM_LOG_WARN 1
#define BLUE_SIM_LOG_ERROR 2
#define BLUE_SIM_LOG_FATAL 3 /* the process aborts after the callback */

/* An opaque simulation (sim::SimWorld). */
typedef struct BlueSim BlueSim;

/* The only struct with pointers; read during blue_sim_create() only. */
typedef struct BlueSimConfig {
  const char* asset_dir;  /* UTF-8 root of the asset tree */
  const char* scene_path; /* UTF-8 .mscn, relative to asset_dir or absolute */
  int32_t worker_threads; /* physics threads besides the caller's, >= 0 */
  int32_t max_players;    /* > 0 */
} BlueSimConfig;

typedef struct BlueVec3 {
  float x;
  float y;
  float z;
} BlueVec3; /* 12 bytes */

typedef struct BlueQuat {
  float x;
  float y;
  float z;
  float w;
} BlueQuat; /* 16 bytes; unit rotation */

/* sim::UserCommand. Bit masks as in sim/user_command.h (wire protocol). */
typedef struct BlueUserCommand {
  uint32_t tick;
  uint32_t held;    /* buttons::held bits */
  uint32_t pressed; /* buttons::pressed bits */
  float yaw;
  float pitch;
  int8_t select_slot;    /* 0 pistol, 1 rifle, -1 none */
  uint8_t interact_kind; /* BLUE_SIM_INTERACT_* */
  uint8_t _pad[2];
  uint32_t interact_id;
} BlueUserCommand; /* 28 bytes */

/* weapon::FirearmState. */
typedef struct BlueFirearmState {
  uint8_t fire_mode; /* 0 semi, 1 auto, 2 double action */
  uint8_t magazine_inserted;
  uint8_t chambered;
  uint8_t bolt_locked;
  uint8_t empty_reload;
  uint8_t pull_used;
  int8_t reload_stage;
  uint8_t spare_count; /* valid entries of spares, <= 8 */
  int16_t magazine;
  int16_t incoming_magazine; /* -1: none being seated */
  int16_t spares[8];
  uint8_t spares_dropped;
  uint8_t _pad[3];
  float reload_elapsed;  /* s */
  float reload_duration; /* s, 0 when not reloading */
  float cycle_remaining; /* s */
} BlueFirearmState;      /* 44 bytes */

/* One loadout slot (weapon::LoadoutSlotState plus the weapon's key). */
typedef struct BlueLoadoutSlot {
  uint8_t present; /* the rest is 0 when not */
  uint8_t _pad[3];
  uint32_t weapon_key;
  BlueFirearmState firearm;
  float scope_magnification;
  float scope_zero; /* m */
} BlueLoadoutSlot;  /* 60 bytes */

/* sim::PlayerState of player `id`. Booleans are 0 or 1. */
typedef struct BluePlayerState {
  uint32_t id;
  BlueVec3 position; /* feet */
  BlueVec3 velocity;
  float jump_speed;
  int32_t jump_age;      /* ticks */
  int32_t jump_cooldown; /* ticks */
  float yaw;
  float pitch;
  float eye_height; /* m above the feet */
  int16_t health;
  uint16_t respawn_ticks;
  uint32_t shots_fired;
  uint32_t deaths;
  uint8_t jump_pending;
  uint8_t crouching;
  uint8_t on_ground;
  uint8_t sprinting;
  uint8_t aiming;
  uint8_t alive;
  int8_t active_slot;   /* weapon in hand, -1 empty hands */
  int8_t selected_slot; /* switch target, -1 none */
  float raise;          /* linear switch progress [0, 1] */
  uint8_t wait_for_release;
  uint8_t _pad[3];
  BlueLoadoutSlot slots[2]; /* 0 pistol, 1 rifle */
} BluePlayerState;          /* 200 bytes */

/* weapon::Segment: one substep of one bullet, swept against the world. */
typedef struct BlueSegment {
  int32_t projectile; /* bullet id, stable while it flies */
  int32_t shot_number;
  uint32_t owner;       /* shooting player */
  float world_fraction; /* of delta where the world stops it; 1: no hit */
  BlueVec3 origin;
  BlueVec3 delta;
  BlueVec3 velocity; /* at the segment start */
  float mass;        /* kg */
  BlueVec3 world_normal;
  uint32_t world_instance; /* scene instance hit, 0: none or not a scene
                              body */
} BlueSegment;             /* 72 bytes */

/* A player hit by a segment (sim::ShotHit). */
typedef struct BlueShotHit {
  int32_t segment; /* index into the last sweep's segments */
  uint32_t victim;
  float fraction; /* along the segment's delta, [0, 1] */
  /* Of the tested capsule's height above its feet; negative: measure the
   * hit point against the victim's current capsule. */
  float height_fraction;
} BlueShotHit; /* 16 bytes */

typedef struct BlueCapsule {
  BlueVec3 feet; /* bottom centre */
  float height;
  float radius;
} BlueCapsule; /* 20 bytes */

typedef struct BlueRayHit {
  BlueVec3 point;
  BlueVec3 normal;
  float fraction;      /* of max_distance */
  uint32_t instance;   /* scene instance owning the body, 0: none */
  uint32_t dropped_id; /* dropped weapon hit, 0: none */
} BlueRayHit;          /* 36 bytes */

/* A dynamic scene body (instance's own prefab). */
typedef struct BlueBodyState {
  uint32_t instance;
  uint8_t active; /* awake */
  uint8_t _pad[3];
  BlueVec3 position; /* body origin */
  BlueQuat rotation;
  BlueVec3 linear_velocity;
  BlueVec3 angular_velocity;
} BlueBodyState; /* 60 bytes */

/* A weapon lying in (or flying through) the world. */
typedef struct BlueDroppedWeapon {
  uint32_t id;
  uint32_t weapon_key;
  BlueVec3 position; /* body origin */
  BlueQuat rotation;
  BlueVec3 scale;
  BlueVec3 linear_velocity;  /* m/s; 0 without a body */
  BlueVec3 angular_velocity; /* rad/s; 0 without a body */
  int16_t magazine;          /* rounds in the inserted magazine */
  uint8_t chambered;         /* a round is chambered */
  uint8_t active;            /* its body is awake (moving) */
} BlueDroppedWeapon;         /* 76 bytes */

typedef struct BluePickup {
  uint32_t instance;
  uint32_t weapon_key;
  uint8_t taken;
  uint8_t _pad[3];
  BlueVec3 position;
  BlueQuat rotation;
} BluePickup; /* 40 bytes */

/* sim::SimEvent (see sim/sim_world.h). Unused fields are 0.
 *   SHOT:           player, id shot number, weapon_key, position origin,
 *                   vector velocity.
 *   IMPACT:         id scene instance (0 none), value shot number,
 *                   position point, vector surface normal.
 *   DAMAGE:         player victim, other attacker, id shot number,
 *                   weapon_key, zone, value damage, health after, position
 *                   point, vector bullet velocity. Also the attacker's hit
 *                   confirmation (hit marker).
 *   DEATH:          player victim, other attacker and weapon_key of the
 *                   killing shot (both 0: no attacker, e.g. a fall below
 *                   the kill plane), position feet.
 *   RESPAWN:        player, position feet, yaw.
 *   WEAPON_DROPPED: player, id dropped item, weapon_key, value rounds in
 *                   the magazine, chambered.
 *   DROPPED_TAKEN:  player, id dropped item, weapon_key.
 *   PICKUP_TAKEN:   player, id scene instance, weapon_key.
 *   DROPPED_REMOVED: id dropped item, weapon_key: it left the world without
 *                   a taker (lifetime over, the cap, the kill plane). */
typedef struct BlueSimEvent {
  uint8_t type;      /* BLUE_SIM_EVENT_* */
  uint8_t zone;      /* BLUE_SIM_ZONE_*; damage only */
  uint8_t chambered; /* weapon dropped only */
  uint8_t _pad;
  uint32_t tick;
  uint32_t player;
  uint32_t other;
  uint32_t id;
  uint32_t weapon_key;
  int32_t value;
  int32_t health;
  float yaw; /* respawn only, radians */
  BlueVec3 position;
  BlueVec3 vector;
} BlueSimEvent; /* 60 bytes */

typedef void (*BlueSimLogCallback)(void* user, int32_t level,
                                   const char* message);

/* Returns BLUE_SIM_ABI_VERSION of the library. */
int32_t blue_sim_abi_version(void) BLUE_SIM_NOEXCEPT;

/* Returns sizeof the struct BLUE_SIM_STRUCT_*, or
 * BLUE_SIM_ERR_INVALID_ARGUMENT for an unknown id. */
int32_t blue_sim_struct_size(int32_t struct_id) BLUE_SIM_NOEXCEPT;

/* Loads the scene and builds the simulation into *out. On failure *out is
 * NULL and, when err is not NULL, a NUL-terminated message (truncated to
 * err_cap bytes) is written to err. */
int32_t blue_sim_create(const BlueSimConfig* config, BlueSim** out, char* err,
                        int32_t err_cap) BLUE_SIM_NOEXCEPT;

/* Destroys `sim`. BLUE_SIM_ERR_NULL for NULL. */
int32_t blue_sim_destroy(BlueSim* sim) BLUE_SIM_NOEXCEPT;

/* Copies the message of the last failure on `sim` (empty if none) into
 * buffer (NUL-terminated, truncated to cap bytes; buffer may be NULL with
 * cap 0) and returns its full length without the NUL. */
int32_t blue_sim_last_error(const BlueSim* sim, char* buffer,
                            int32_t cap) BLUE_SIM_NOEXCEPT;

/* Sets the process-wide sink of every log line of this library and the
 * engine code under it (core::log; NULL restores stdout/stderr). The
 * callback gets a NUL-terminated message without a level prefix or newline,
 * is never called on two threads at once but may be called from physics
 * worker threads, and must not call back into blue_sim. Set it before
 * blue_sim_create() starts worker threads. */
int32_t blue_sim_set_log_callback(BlueSimLogCallback callback,
                                  void* user) BLUE_SIM_NOEXCEPT;

/* Writes the content hash client and server must agree on to *out. */
int32_t blue_sim_content_hash(const BlueSim* sim,
                              uint64_t* out) BLUE_SIM_NOEXCEPT;

/* Adds player `id` at a spawn point (BLUE_SIM_ERR_PLAYER_REFUSED: id in
 * use, sim full, or its body could not be made). */
int32_t blue_sim_add_player(BlueSim* sim, uint32_t id) BLUE_SIM_NOEXCEPT;

int32_t blue_sim_remove_player(BlueSim* sim, uint32_t id) BLUE_SIM_NOEXCEPT;

/* Stores `command` for player `id`'s next step. */
int32_t blue_sim_apply_command(BlueSim* sim, uint32_t id,
                               const BlueUserCommand* command)
    BLUE_SIM_NOEXCEPT;

/* Runs tick `tick`. */
int32_t blue_sim_step(BlueSim* sim, uint32_t tick) BLUE_SIM_NOEXCEPT;

/* Sweeps the bullets over one tick (once per tick: calling it again before
 * the resolve returns the same segments) and copies the segments to out.
 * Returns their count; BLUE_SIM_ERR_BUFFER_TOO_SMALL when cap is smaller
 * (out may be NULL with cap 0 to ask for the count). */
int32_t blue_sim_projectiles_sweep(BlueSim* sim, BlueSegment* out,
                                   int32_t cap) BLUE_SIM_NOEXCEPT;

/* Resolves the sweep with `count` player hits (hits may be NULL for 0):
 * damage, deaths, impulses, events. Hits naming no segment are ignored. */
int32_t blue_sim_projectiles_resolve(BlueSim* sim, const BlueShotHit* hits,
                                     int32_t count) BLUE_SIM_NOEXCEPT;

/* Writes the hits of the current sweep's segments on the players' current
 * capsules (shooter excluded) to out and returns their count, like
 * blue_sim_projectiles_sweep(). For offline play and tests; the server
 * tests lag-compensated capsules itself. */
int32_t blue_sim_hit_capsules_now(const BlueSim* sim, BlueShotHit* out,
                                  int32_t cap) BLUE_SIM_NOEXCEPT;

/* Writes player `id`'s current capsule to *out. */
int32_t blue_sim_player_capsule(const BlueSim* sim, uint32_t id,
                                BlueCapsule* out) BLUE_SIM_NOEXCEPT;

/* Casts a ray against the world's bodies (players are not bodies). Returns
 * 1 and fills *out on a hit, 0 without one. */
int32_t blue_sim_cast_ray(const BlueSim* sim, const BlueVec3* origin,
                          const BlueVec3* direction, float max_distance,
                          BlueRayHit* out) BLUE_SIM_NOEXCEPT;

/* Read-outs: copy every entry (players by id, bodies in scene order,
 * dropped weapons by id, pickups by instance) to out and return the count,
 * or BLUE_SIM_ERR_BUFFER_TOO_SMALL (out may be NULL with cap 0 to ask). */
int32_t blue_sim_read_players(const BlueSim* sim, BluePlayerState* out,
                              int32_t cap) BLUE_SIM_NOEXCEPT;
int32_t blue_sim_read_bodies(const BlueSim* sim, BlueBodyState* out,
                             int32_t cap) BLUE_SIM_NOEXCEPT;
int32_t blue_sim_read_dropped_weapons(const BlueSim* sim,
                                      BlueDroppedWeapon* out,
                                      int32_t cap) BLUE_SIM_NOEXCEPT;
int32_t blue_sim_read_pickups(const BlueSim* sim, BluePickup* out,
                              int32_t cap) BLUE_SIM_NOEXCEPT;

/* Moves up to cap of the oldest queued events to out and returns how many
 * (0 when none are queued); the rest stay queued. */
int32_t blue_sim_poll_events(BlueSim* sim, BlueSimEvent* out,
                             int32_t cap) BLUE_SIM_NOEXCEPT;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* BLUE_SIM_C_API_BLUE_SIM_H_ */
