#ifndef BLUE_WEAPON_PROJECTILE_SYSTEM_H_
#define BLUE_WEAPON_PROJECTILE_SYSTEM_H_

#include <cstdint>
#include <span>
#include <vector>

#include <glm/vec3.hpp>

#include "physics/physics_world.h"

namespace blue::weapon {

// A bullet in flight. SI units, world space.
struct Projectile {
  glm::vec3 position{0.0f};
  // Position at the start of the most recent Update (the spawn point for a
  // bullet not yet updated): the segment to draw a tracer along.
  glm::vec3 previous_position{0.0f};
  glm::vec3 velocity{0.0f};  // m/s
  float mass = 0.0f;         // kg
  float drag = 0.0f;         // 1/m, deceleration = drag * |v|^2
  float age = 0.0f;          // s of simulated flight
  float distance = 0.0f;     // m travelled along the path
  int shot_number = 0;       // as given to Spawn
  uint32_t owner = 0;        // as given to Spawn, opaque
  int id = 0;                // unique among live bullets, > 0; Segment
};

// A bullet that hit a body during the most recent Update.
struct Impact {
  glm::vec3 point{0.0f};
  glm::vec3 normal{0.0f, 1.0f, 0.0f};  // surface normal of the hit body, unit
  glm::vec3 velocity{0.0f};            // bullet velocity at the hit, m/s
  float mass = 0.0f;                   // kg
  physics::BodyId body{};
  float flight_time = 0.0f;  // s from the shot (spawn age included) to the hit
  float distance = 0.0f;     // m travelled along the path to the hit
  int shot_number = 0;       // of the bullet, as given to Spawn
};

// One integration substep of one bullet, recorded by
// ProjectileSystem::Sweep: the bullet moves from `origin` to
// `origin + delta` unless the world (or a player, see PlayerHit) stops it
// first.
struct Segment {
  int projectile = 0;   // Projectile::id, stable while the bullet flies
  int shot_number = 0;  // of the bullet, as given to Spawn
  uint32_t owner = 0;   // of the bullet, as given to Spawn
  glm::vec3 origin{0.0f};
  glm::vec3 delta{0.0f};
  // Fraction of `delta` at which the world stops the bullet; 1 = no world
  // hit on this segment.
  float world_fraction = 1.0f;
  glm::vec3 velocity{0.0f};      // m/s at the segment start
  float mass = 0.0f;             // kg
  physics::BodyId world_body{};  // the hit body; meaningless without a hit
  glm::vec3 world_normal{0.0f};  // of the hit surface; 0 without a hit
};

// A player hit by a Segment, as found by the caller (who alone knows the
// players' capsules, e.g. lag compensated on the server).
struct PlayerHit {
  int segment = 0;        // index into ProjectileSystem::segments()
  uint32_t victim = 0;    // opaque, reported back in PlayerImpact
  float fraction = 0.0f;  // along the segment's delta, [0, 1]
};

// A bullet that hit a player during the most recent Resolve.
struct PlayerImpact {
  uint32_t victim = 0;  // as given in PlayerHit
  glm::vec3 point{0.0f};
  glm::vec3 velocity{0.0f};  // bullet velocity at the hit, m/s
  float mass = 0.0f;         // kg
  int shot_number = 0;       // of the bullet, as given to Spawn
  uint32_t owner = 0;        // of the bullet, as given to Spawn
  float flight_time = 0.0f;  // s from the shot (spawn age included) to the hit
  float distance = 0.0f;     // m travelled along the path to the hit
};

// One integration step of a bullet: how far it moves and its velocity
// afterwards.
struct BallisticStep {
  glm::vec3 displacement{0.0f};  // m
  glm::vec3 velocity{0.0f};      // m/s at the end of the step
};

// Advances a bullet moving with `velocity` by `h` seconds under `gravity`
// and quadratic `drag` (a = g - drag * |v| * v) with the midpoint method,
// exact without drag: ProjectileSystem's integration step, shared with the
// zeroing (weapon/zeroing.h) so a computed zero matches the bullets flown.
[[nodiscard]] BallisticStep IntegrateBallisticStep(glm::vec3 velocity,
                                                   glm::vec3 gravity,
                                                   float drag, float h);

// Limits and integration settings of a ProjectileSystem.
struct ProjectileTuning {
  int capacity = 256;            // bullets in flight; the oldest is dropped
  float max_age = 6.0f;          // s before a bullet expires
  float max_distance = 3000.0f;  // m travelled before a bullet expires
  float max_substep = 1.0f / 240.0f;  // s, longest integration step
};

// Simulates bullets as point masses under gravity and quadratic air drag
// (a = g - drag * |v| * v) and sweeps each integration substep against the
// physics world with PhysicsWorld::CastRay, so even 948 m/s bullets cannot
// pass through a thin wall. Bullets are deliberately not physics bodies.
//
// Each Update splits its time into equal substeps no longer than
// `max_substep` and integrates them with the midpoint method, which is exact
// without drag, so trajectories do not depend on the frame rate beyond
// rounding. The first hit along a substep removes the bullet, records an
// Impact and, for a dynamic body, applies the impulse mass * velocity at the
// hit point. Bullets also expire by age and distance.
//
// Order within a frame: Spawn the frame's shots (with Firearm's shot ages),
// then Update. A bullet spawned since the last Update is advanced by its
// spawn `age` in the next Update, not by that Update's dt.
//
// Update is Sweep followed by Resolve with no player hits. Splitting them
// lets a caller test the swept segments against targets the physics world
// does not hold (players' capsules) in between: Sweep records every substep
// as a Segment without changing any bullet, the caller turns its own hits
// into PlayerHits, and Resolve applies the earliest hit of each bullet (a
// player's or the world's) and advances the survivors. Call Resolve once,
// right after Sweep; Spawn or Clear in between discards the sweep (Resolve
// then only clears the hit lists).
//
// Storage is reserved at construction (segments for 8 substeps per bullet)
// and kept, so once it has grown to a frame's needs Spawn, Update, Sweep and
// Resolve never allocate.
// The physics::Character is invisible to CastRay (it has no rigid body), so a
// bullet never hits the shooter's own capsule. Copyable. Not thread-safe.
class ProjectileSystem {
 public:
  explicit ProjectileSystem(const ProjectileTuning& tuning = {});
  ProjectileSystem(const ProjectileSystem&) = default;
  ProjectileSystem& operator=(const ProjectileSystem&) = default;

  // Adds a bullet at `position` moving with `velocity` (m/s), `mass` kg and
  // `drag` 1/m, fired `age` seconds before the end of the current frame
  // (negative counts as 0). `shot_number` is carried to the bullet's Impact
  // so hits can be matched to shots. At capacity the oldest bullet is
  // dropped.
  // `owner` is opaque, carried to the bullet's segments and player impacts.
  void Spawn(glm::vec3 position, glm::vec3 velocity, float mass, float drag,
             float age = 0.0f, int shot_number = 0, uint32_t owner = 0);

  // Advances every bullet by `dt` seconds (fresh ones by their spawn age) under
  // `gravity` (m/s^2), resolving hits against `world`. Replaces impacts().
  void Update(physics::PhysicsWorld& world, glm::vec3 gravity, float dt);

  // First phase of Update: integrates every bullet over `dt` as Update does
  // and records each substep as a Segment, cast against `world`. A bullet's
  // segments are consecutive, in flight order, and end at its first world
  // hit or where it expires. Changes no bullet. Replaces segments().
  void Sweep(const physics::PhysicsWorld& world, glm::vec3 gravity, float dt);

  // Second phase of Update: for each bullet the earliest hit along its
  // segments wins. A player hit counts when its fraction is below its
  // segment's world_fraction; the first such segment, then the smallest
  // fraction, wins. It removes the bullet and is reported in
  // player_impacts(). Otherwise a world hit removes the bullet exactly as
  // Update does (Impact; impulse on a dynamic body). Survivors advance to
  // where the sweep left them. Hits naming no segment of the sweep are
  // ignored. Replaces impacts() and player_impacts(); empties segments().
  void Resolve(physics::PhysicsWorld& world,
               std::span<const PlayerHit> player_hits);

  [[nodiscard]] std::span<const Projectile> projectiles() const {
    return projectiles_;
  }
  // Hits of the most recent Update, in no particular order.
  [[nodiscard]] std::span<const Impact> impacts() const { return impacts_; }
  // Player hits of the most recent Resolve, in bullet order.
  [[nodiscard]] std::span<const PlayerImpact> player_impacts() const {
    return player_impacts_;
  }
  // Segments of the most recent Sweep; empty after Resolve, Spawn or Clear.
  [[nodiscard]] std::span<const Segment> segments() const { return segments_; }
  [[nodiscard]] const ProjectileTuning& tuning() const { return tuning_; }

  // Removes every bullet, impact and segment.
  void Clear();

 private:
  // What the sweep found for one bullet (indexed like projectiles_).
  struct Flight {
    Projectile end;      // after its last segment (at the world hit, if any)
    bool alive = false;  // flies on after this sweep
    bool world_hit = false;  // its last segment hit the world
    int last_segment = -1;   // index of that segment when world_hit
    // The winning player hit's segment index and fraction; -1 for none.
    int player_segment = -1;
    float player_fraction = 0.0f;
    uint32_t victim = 0;
  };
  // What Resolve needs of a segment besides the Segment itself.
  struct SegmentTrack {
    int bullet = 0;  // index into projectiles_ and flights_
    glm::vec3 end_velocity{0.0f};
    float start_age = 0.0f;       // s, bullet age at the segment start
    float duration = 0.0f;        // s
    float start_distance = 0.0f;  // m travelled at the segment start
    float length = 0.0f;          // m, |delta|
  };

  // Integrates `bullet` (index `index`) over `seconds`, recording its
  // segments, and returns where it ends up.
  Flight SweepBullet(const physics::PhysicsWorld& world, glm::vec3 gravity,
                     float seconds, int index, Projectile bullet);
  // Forgets the recorded sweep.
  void DiscardSweep();

  ProjectileTuning tuning_;
  std::vector<Projectile> projectiles_;
  std::vector<Impact> impacts_;
  std::vector<PlayerImpact> player_impacts_;
  std::vector<Segment> segments_;
  std::vector<SegmentTrack> tracks_;  // parallel to segments_
  std::vector<Flight> flights_;       // parallel to projectiles_ when swept_
  bool swept_ = false;                // a Sweep awaits its Resolve
  int next_id_ = 1;                   // Projectile::id of the next Spawn
  // The last `fresh_count_` projectiles were spawned since the last Update;
  // their `age` holds the spawn age still to be simulated.
  int fresh_count_ = 0;
};

}  // namespace blue::weapon

#endif  // BLUE_WEAPON_PROJECTILE_SYSTEM_H_
