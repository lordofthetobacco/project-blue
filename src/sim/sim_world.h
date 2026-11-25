#ifndef BLUE_SIM_SIM_WORLD_H_
#define BLUE_SIM_SIM_WORLD_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include "asset/mesh_data.h"
#include "core/result.h"
#include "physics/physics_runtime.h"
#include "physics/physics_world.h"
#include "scene/scene_data.h"
#include "sim/damage.h"
#include "sim/player_sim.h"
#include "sim/player_state.h"
#include "sim/user_command.h"
#include "sim/weapon_table.h"
#include "weapon/dropped_weapons.h"
#include "weapon/parts/part_catalog.h"
#include "weapon/projectile_system.h"
#include "weapon/weapon_def.h"
#include "weapon/weapon_item.h"
#include "world/core/collision_world.h"
#include "world/core/placement.h"
#include "world/core/prefab_store.h"

namespace blue::sim {

// Values of UserCommand::interact_kind (docs/netcode/protocol.md, section
// 8.1). Part of the wire protocol: append only.
namespace interact_kind {
inline constexpr uint8_t kNone = 0;
inline constexpr uint8_t kSceneWeapon = 1;    // interact_id: scene instance
inline constexpr uint8_t kDroppedWeapon = 2;  // interact_id: dropped item id
}  // namespace interact_kind

// A weapon every player carries when they join or respawn: a prefab
// relative to the asset directory and the weapon type it is when the prefab
// carries no `weapon` tag (assets cooked before prefabs had tags; empty: no
// fallback). The last one starts in hand.
struct StartingWeapon {
  std::string path;
  std::string fallback_type;
};

// The single-player game's starting loadout: glock17.pbo, then m16.pbo.
[[nodiscard]] std::vector<StartingWeapon> DefaultStartingWeapons();

// How to build a SimWorld.
struct SimWorldOptions {
  // Root of the asset tree (cooked prefabs, weapons/catalog/).
  std::string asset_dir;
  // The .mscn to simulate, relative to `asset_dir` (or absolute).
  std::string scene_path;
  // Physics worker threads besides the caller's (PhysicsWorldOptions).
  int worker_threads = 0;
  // Most players at once; AddPlayer() refuses more.
  int max_players = 32;
  std::vector<StartingWeapon> starting_weapons = DefaultStartingWeapons();
  // Ticks a dropped weapon lies in the world before it despawns (120 s).
  uint32_t dropped_lifetime_ticks = 7200;
  // Most dropped weapons at once; dropping another removes the oldest.
  int max_dropped_weapons = 64;
  // m below the lowest collision bound of the scene: living players whose
  // feet fall below it die, dropped weapons below it are removed and
  // dynamic bodies below it are put to sleep.
  float kill_plane_depth = 50.0f;
};

// Kinds of SimEvent. Values are part of the C ABI: append only.
enum class SimEventType : uint8_t {
  kShot = 0,           // player fired: id shot number, weapon_key, position
                       // origin, vector velocity
  kImpact = 1,         // a bullet hit the world: id instance (0 none),
                       // value shot number, position point, vector surface
                       // normal
  kDamage = 2,         // player was hit by `other`'s bullet: id shot number,
                       // weapon_key of the shooting weapon, zone, value
                       // damage, health left, position point, vector bullet
                       // velocity. Also the shooter's hit confirmation.
  kDeath = 3,          // player was killed by `other` with weapon_key (both
                       // 0: no attacker), position feet
  kRespawn = 4,        // player came back at `position` facing `yaw`
  kWeaponDropped = 5,  // player dropped dropped item `id` (weapon_key) with
                       // value rounds in the magazine and `chambered`
  kDroppedTaken = 6,   // player picked up dropped item `id`
  kPickupTaken = 7,    // player picked up scene pickup instance `id`
  // 8 was kHitConfirmed (ABI 1), a copy of kDamage; retired, never emitted.
  // Dropped item `id` (weapon_key) left the world without a taker: its
  // lifetime ran out, the cap removed it, or it fell below the kill plane.
  kDroppedRemoved = 9,
};

// One thing that happened in the simulation, for the server to broadcast.
// Fields a kind does not use are 0.
struct SimEvent {
  SimEventType type = SimEventType::kShot;
  HitZone zone = HitZone::kTorso;  // kDamage
  bool chambered = false;          // kWeaponDropped
  uint32_t tick = 0;
  uint32_t player = 0;  // who the event is about
  uint32_t other = 0;   // the other player involved
  uint32_t id = 0;      // see SimEventType
  WeaponKey weapon_key = 0;
  int32_t value = 0;
  int32_t health = 0;  // kDamage: health left
  float yaw = 0.0f;    // kRespawn: radians
  glm::vec3 position{0.0f};
  glm::vec3 vector{0.0f};
};

// One player's state, as read out.
struct PlayerReadout {
  uint32_t id = 0;
  PlayerState state;
};

// One dynamic scene body (an instance's own prefab asking for dynamic
// motion), as read out.
struct BodyReadout {
  scene::InstanceId instance = scene::kNoInstance;
  physics::BodyState state;
};

// One weapon lying in the world, as read out.
struct DroppedReadout {
  uint32_t id = 0;
  WeaponKey weapon_key = 0;
  physics::Pose pose;
  glm::vec3 scale{1.0f};
  glm::vec3 linear_velocity{0.0f};   // m/s, 0 without a body
  glm::vec3 angular_velocity{0.0f};  // rad/s, 0 without a body
  int magazine = 0;                  // rounds in the inserted magazine
  bool chambered = false;
  bool active = false;  // its body is awake (moving)
};

// One weapon placed in the scene as a pick-up, as read out.
struct PickupReadout {
  scene::InstanceId instance = scene::kNoInstance;
  WeaponKey weapon_key = 0;
  physics::Pose pose;  // the instance's own prefab origin
  bool taken = false;
};

// A player capsule: feet (bottom centre), total height and radius (m).
struct Capsule {
  glm::vec3 feet{0.0f};
  float height = 0.0f;
  float radius = 0.0f;
};

// A player hit by a swept segment (weapon::PlayerHit) plus where on the
// capsule it was hit: `height_fraction` of the tested capsule's height above
// its feet, or negative to measure the point against the victim's current
// capsule (only right when the hit was found against it).
struct ShotHit {
  int segment = 0;
  uint32_t victim = 0;
  float fraction = 0.0f;
  float height_fraction = -1.0f;
};

// Where a ray cast into the simulated world stopped.
struct SimRayHit {
  glm::vec3 point{0.0f};
  glm::vec3 normal{0.0f};
  float fraction = 0.0f;  // of the cast's max distance
  // The scene instance owning the hit body, or kNoInstance.
  scene::InstanceId instance = scene::kNoInstance;
  uint32_t dropped_id = 0;  // the dropped weapon hit, or 0
};

// Returns the first fraction in [0, 1] at which the segment from `origin`
// along `delta` touches `capsule`, or nullopt; 0 when it starts inside.
[[nodiscard]] std::optional<float> SegmentHitsCapsule(glm::vec3 origin,
                                                      glm::vec3 delta,
                                                      const Capsule& capsule);

// The whole authoritative game simulation the dedicated server runs: the
// scene's collision world (PrefabStore, ResolvePlacements, CollisionWorld,
// dynamic bodies simulated), the weapon table, the scene's weapon pick-ups,
// dropped weapons, bullets and the players (by id).
//
// A server tick:
//   ApplyCommand() for every command received for the tick;
//   Step(tick): respawns due, players in id order (PlayerSim::Step; a
//     player without a command repeats its last held buttons, with no
//     pressed edges), drop / pick-up requests (validated for reach, aim and
//     line of sight with the game's interaction limits), one physics step,
//     then this tick's shots become bullets;
//   SweepProjectiles(), the caller's player hits (lag compensated, or
//     HitCapsulesNow()), ResolveProjectiles(): damage, deaths, impulses;
//   read-outs and PollEvents() for the snapshot.
//
// Deterministic for the same inputs: no wall-clock time, no global state,
// players and items iterated in id order. No allocation per tick once the
// event queue and bullet buffers have grown. Not thread-safe; neither
// copyable nor movable.
class SimWorld {
 public:
  // Loads the scene and builds everything. `runtime` must outlive the
  // world. Errors name the failing file or step; never terminates.
  [[nodiscard]] static core::Result<std::unique_ptr<SimWorld>, std::string>
  Create(const physics::PhysicsRuntime& runtime,
         const SimWorldOptions& options);

  SimWorld(const SimWorld&) = delete;
  SimWorld& operator=(const SimWorld&) = delete;
  ~SimWorld();

  // Adds player `id` at a spawn point with full health and the starting
  // loadout. Returns false for an id in use, when max_players are in, or
  // when the character cannot be created (logged).
  bool AddPlayer(uint32_t id);
  // Removes player `id` (the weapons it carries vanish). False if unknown.
  bool RemovePlayer(uint32_t id);
  // Stores `command` for the next Step(). False for an unknown player.
  bool ApplyCommand(uint32_t id, const UserCommand& command);
  // Runs tick `tick`; see the class comment.
  void Step(uint32_t tick);

  // Sweeps the bullets over one tick (weapon::ProjectileSystem::Sweep) and
  // returns the segments, valid until ResolveProjectiles() or the next
  // Step(). Calling it again before ResolveProjectiles() returns the same
  // segments without sweeping again.
  std::span<const weapon::Segment> SweepProjectiles();
  // Resolves the sweep with `hits` (weapon::ProjectileSystem::Resolve):
  // bullets hitting players damage them (ComputeDamage; self hits and hits
  // on dead or unknown players are ignored), lethal damage kills (the
  // weapon in hand is dropped, respawn after kRespawnTicks), world hits
  // push dynamic bodies. Without a sweep since the last call it sweeps
  // first.
  void ResolveProjectiles(std::span<const ShotHit> hits);
  // Writes to `out` (cleared first) every hit of the current sweep's
  // segments on the players' current capsules, the shooter's own excluded.
  void HitCapsulesNow(std::vector<ShotHit>& out) const;

  // Returns the capsule of player `id`, or nullopt.
  [[nodiscard]] std::optional<Capsule> CurrentCapsule(uint32_t id) const;
  // Casts a ray against the world's bodies (players are not bodies).
  [[nodiscard]] std::optional<SimRayHit> CastRay(glm::vec3 origin,
                                                 glm::vec3 direction,
                                                 float max_distance) const;

  // Returns the scene instance owning `body`, or scene::kNoInstance.
  [[nodiscard]] scene::InstanceId InstanceOf(physics::BodyId body) const;

  // Read-outs: each writes min(out.size(), total) entries, in id order (for
  // bodies: scene order), and returns the total.
  size_t ReadPlayers(std::span<PlayerReadout> out) const;
  size_t ReadBodies(std::span<BodyReadout> out) const;
  size_t ReadDroppedWeapons(std::span<DroppedReadout> out) const;
  size_t ReadPickups(std::span<PickupReadout> out) const;
  // Moves up to out.size() of the oldest queued events into `out` and
  // returns how many; the rest stay queued.
  size_t PollEvents(std::span<SimEvent> out);
  [[nodiscard]] size_t pending_events() const {
    return events_.size() - events_read_;
  }

  // The weapon table's content hash combined (64-bit FNV-1a) with the bytes
  // of the scene file: client and server must agree on it.
  [[nodiscard]] uint64_t content_hash() const { return content_hash_; }
  [[nodiscard]] const WeaponTable& weapon_table() const { return table_; }
  [[nodiscard]] const weapon::WeaponLibrary& weapon_library() const {
    return library_;
  }
  [[nodiscard]] const scene::SceneData& scene() const { return scene_; }
  // Player `id`, or null. Valid until it is removed.
  [[nodiscard]] const PlayerSim* player(uint32_t id) const;
  [[nodiscard]] size_t player_count() const { return players_.size(); }
  [[nodiscard]] uint32_t tick() const { return tick_; }
  // Height (m) of the kill plane: 50 m (kill_plane_depth) below the lowest
  // collision bound of the scene.
  [[nodiscard]] float kill_plane_y() const { return kill_plane_y_; }

 private:
  // One player and its command bookkeeping.
  struct Player {
    uint32_t id = 0;
    std::unique_ptr<PlayerSim> sim;
    UserCommand pending;  // valid when has_pending
    bool has_pending = false;
    UserCommand last;  // the last command executed
  };
  // A shot of this tick, waiting to become a bullet.
  struct PendingShot {
    uint32_t owner = 0;
    ShotEvent shot;
  };
  // The weapon that fired a bullet in flight (bullets are named by owner
  // and shot number, unique per owner).
  struct ShotWeapon {
    uint32_t owner = 0;
    int shot_number = 0;
    WeaponKey weapon_key = 0;
  };
  // Where on the capsule the caller says a bullet hit a player.
  struct HitHeight {
    uint32_t owner = 0;
    int shot_number = 0;
    uint32_t victim = 0;
    float height_fraction = -1.0f;
  };
  // One scene weapon pick-up.
  struct Pickup {
    scene::InstanceId instance = scene::kNoInstance;
    weapon::WeaponDefId def{};
    WeaponKey key = 0;
    std::optional<physics::BodyId> body;  // its own prefab's body
    glm::mat4 placement{1.0f};            // prefab space -> world at rest
    glm::vec3 scale{1.0f};
    asset::Bounds bounds;  // its own prefab's, prefab space
    bool taken = false;
  };

  SimWorld(const physics::PhysicsRuntime& runtime,
           const SimWorldOptions& options);

  // Loads and builds everything Create() promises.
  [[nodiscard]] core::Result<void, std::string> Build(
      const SimWorldOptions& options);
  // Loads the starting weapons' prefabs (adding fallback tags) and returns
  // their asset-relative paths.
  [[nodiscard]] core::Result<std::vector<std::string>, std::string>
  LoadStartingWeapons(const SimWorldOptions& options);
  // Appends the scene's weapon pick-ups among `placements` to pickups_
  // (def and key unset) and returns their prefabs' asset-relative paths,
  // in the same order.
  [[nodiscard]] std::vector<std::string> FindPickups(
      std::span<const world::Placement> placements);

  [[nodiscard]] Player* FindPlayer(uint32_t id);
  [[nodiscard]] const Player* FindPlayer(uint32_t id) const;
  [[nodiscard]] weapon::Loadout StartingLoadout() const;
  // Returns the feet of every live player but `except`.
  void CollectOccupied(uint32_t except);
  // Runs the drop and pick-up requests of `command` for `player`.
  void Interact(Player& player, const UserCommand& command);
  // Takes `weapon` into `player`'s loadout, tossing out what it replaces.
  void TakeWeapon(Player& player, weapon::WeaponItem weapon);
  // Puts `weapon` into the world as thrown by `player` (from the aimed
  // hold, or tossed from the chest) and emits kWeaponDropped.
  void ThrowWeapon(const PlayerSim& player, weapon::WeaponItem weapon,
                   bool toss);
  // Returns whether `player` may interact with the box placed by `matrix`
  // (`bounds` in prefab space): within reach, aimed at and in sight, the
  // item's own body (`own_body`) not hiding it.
  [[nodiscard]] bool CanReach(const PlayerSim& player, const glm::mat4& matrix,
                              const asset::Bounds& bounds,
                              std::optional<physics::BodyId> own_body) const;
  // Returns the world matrix of `pickup` now.
  [[nodiscard]] glm::mat4 PickupMatrix(const Pickup& pickup) const;
  [[nodiscard]] physics::Pose PickupPose(const Pickup& pickup) const;
  // Returns the key of the weapon that fired bullet `shot_number` of
  // `owner`, or 0 when it is no longer known.
  [[nodiscard]] WeaponKey ShotWeaponKey(uint32_t owner, int shot_number) const;
  // Forgets the weapons of bullets no longer in flight.
  void PruneShotWeapons();
  // Applies one bullet hit on a player.
  void ApplyPlayerImpact(const weapon::PlayerImpact& impact);
  // Kills `victim`, dropping the weapon in hand unless `drop_weapon` is
  // false; `attacker` (0: none) shot it with the weapon `weapon_key`.
  void KillPlayer(Player& victim, uint32_t attacker, WeaponKey weapon_key,
                  bool drop_weapon = true);
  // Removes dropped item `id` without a taker and emits kDroppedRemoved.
  void RemoveDropped(uint32_t id);
  // Despawns dropped weapons whose lifetime ran out, in drop order.
  void ExpireDroppedWeapons();
  // Kills living players below the kill plane, removes dropped weapons
  // below it and puts the dynamic bodies below it to sleep.
  void ApplyKillPlane();
  // Returns the lowest world-space y of any collider of `placements`.
  [[nodiscard]] float LowestColliderY(
      std::span<const world::Placement> placements) const;
  void Emit(const SimEvent& event);

  physics::PhysicsWorld physics_;
  world::PrefabStore store_;
  std::string asset_root_;  // absolute
  scene::SceneData scene_;
  std::optional<weapon::parts::PartCatalog> catalog_;
  weapon::WeaponLibrary library_;
  WeaponTable table_;
  std::vector<weapon::WeaponDefId> starting_defs_;
  world::CollisionWorld collision_;
  weapon::DroppedWeapons dropped_;
  weapon::ProjectileSystem projectiles_;
  std::vector<Pickup> pickups_;  // by instance id
  std::vector<Player> players_;  // by id
  int max_players_ = 0;
  uint64_t content_hash_ = 0;
  uint32_t tick_ = 0;
  bool swept_ = false;  // a sweep awaits its resolve
  std::vector<SimEvent> events_;
  size_t events_read_ = 0;          // events_[0, events_read_) were polled
  std::vector<PendingShot> shots_;  // this tick's, reused
  // One per bullet in flight (pruned every Step()), in spawn order.
  std::vector<ShotWeapon> shot_weapons_;
  std::vector<glm::vec3> occupied_;  // reused by CollectOccupied()
  // Reused by ResolveProjectiles().
  std::vector<weapon::PlayerHit> player_hits_;
  std::vector<HitHeight> hit_heights_;
  // When each dropped weapon was dropped, in drop order (ids are not: Add()
  // reuses free ones).
  struct DroppedAge {
    uint32_t id = 0;
    uint32_t tick = 0;
  };
  std::vector<DroppedAge> dropped_ages_;
  uint32_t dropped_lifetime_ticks_ = 0;
  size_t max_dropped_weapons_ = 0;
  float kill_plane_depth_ = 0.0f;
  float kill_plane_y_ = 0.0f;
};

}  // namespace blue::sim

#endif  // BLUE_SIM_SIM_WORLD_H_
