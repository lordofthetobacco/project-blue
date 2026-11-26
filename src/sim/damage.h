#ifndef BLUE_SIM_DAMAGE_H_
#define BLUE_SIM_DAMAGE_H_

#include <cstdint>
#include <span>

#include <glm/vec3.hpp>

#include "physics/physics_world.h"
#include "scene/scene_data.h"

// Bullet damage, hit zones and where dead players come back.

namespace blue::sim {

// Hit points per kg*m/s of bullet momentum relative to the target: a 4 g
// rifle bullet at 948 m/s does about 34 HP to the torso.
inline constexpr float kDamagePerMomentum = 9.0f;

// Ticks a dead player waits before respawning (5 s at 60 Hz).
inline constexpr uint16_t kRespawnTicks = 300;

// Share of the capsule height, from the top, that counts as the head, and
// from the bottom, that counts as the legs; the torso is in between.
inline constexpr float kHeadShare = 0.15f;
inline constexpr float kLegShare = 0.45f;

// Where a bullet hit a player. Values are part of the C ABI (BlueSimEvent);
// append only.
enum class HitZone : uint8_t { kHead = 0, kTorso = 1, kLegs = 2 };

// Returns the zone of a hit `height_fraction` of the capsule's height above
// its feet (0 feet, 1 top of the head; clamped; NaN counts as the torso).
[[nodiscard]] HitZone ZoneOfHeight(float height_fraction);

// Returns the damage multiplier of `zone`: head 4, torso 1, legs 0.6.
[[nodiscard]] float ZoneMultiplier(HitZone zone);

// Returns the damage (HP, rounded to the nearest whole point, at least 1
// for any positive momentum) of a bullet of `mass` kg hitting with
// `bullet_velocity` a target moving with `target_velocity` (m/s) in `zone`:
// kDamagePerMomentum * mass * |bullet_velocity - target_velocity| *
// ZoneMultiplier(zone). 0 for non-finite or non-positive inputs.
[[nodiscard]] int ComputeDamage(float mass, glm::vec3 bullet_velocity,
                                glm::vec3 target_velocity, HitZone zone);

// Geometry of the respawn ring around the scene's PlayerStart.
struct SpawnRing {
  int points = 8;
  float radius = 2.5f;  // m, horizontal distance from the start position
  // m below a candidate (taken at the start position's height) searched for
  // a floor; as far as the game's own spawn probe reaches.
  float probe_distance = 3.0f;
  float clearance_height = 1.8f;  // m kept free above the floor point
  // m: a candidate this close (horizontally) to an occupied position is
  // skipped (two capsule radii).
  float min_separation = 0.7f;
};

// Returns the feet position player `player_id` respawns at after its
// `deaths`-th death. Candidates are `ring.points` points evenly around
// `start` (horizontally), tried in order from an index chosen
// deterministically from player id and death count. A candidate is skipped
// when the way to it from the start position is blocked, no floor is found
// below it within the probe distance, the space above the floor is blocked,
// or it lies within `min_separation` of one of `occupied` (feet of live
// players). Without a usable candidate the start position's floor is used
// (as the single-player game spawns), or the start position lowered by
// `fallback_eye_height` without a floor either.
[[nodiscard]] glm::vec3 ChooseSpawnPoint(const physics::PhysicsWorld& world,
                                         const scene::PlayerStart& start,
                                         uint32_t player_id, uint32_t deaths,
                                         std::span<const glm::vec3> occupied,
                                         float fallback_eye_height,
                                         const SpawnRing& ring = {});

}  // namespace blue::sim

#endif  // BLUE_SIM_DAMAGE_H_
