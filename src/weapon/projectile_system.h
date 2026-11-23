#ifndef BLUE_WEAPON_PROJECTILE_SYSTEM_H_
#define BLUE_WEAPON_PROJECTILE_SYSTEM_H_

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
// All storage is reserved at construction: Spawn and Update never allocate.
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
  void Spawn(glm::vec3 position, glm::vec3 velocity, float mass, float drag,
             float age = 0.0f, int shot_number = 0);

  // Advances every bullet by `dt` seconds (fresh ones by their spawn age) under
  // `gravity` (m/s^2), resolving hits against `world`. Replaces impacts().
  void Update(physics::PhysicsWorld& world, glm::vec3 gravity, float dt);

  [[nodiscard]] std::span<const Projectile> projectiles() const {
    return projectiles_;
  }
  // Hits of the most recent Update, in no particular order.
  [[nodiscard]] std::span<const Impact> impacts() const { return impacts_; }
  [[nodiscard]] const ProjectileTuning& tuning() const { return tuning_; }

  // Removes every bullet and impact.
  void Clear();

 private:
  // Advances `bullet` by `seconds`; returns false when it hit or expired.
  bool Advance(physics::PhysicsWorld& world, glm::vec3 gravity, float seconds,
               Projectile& bullet);

  ProjectileTuning tuning_;
  std::vector<Projectile> projectiles_;
  std::vector<Impact> impacts_;
  // The last `fresh_count_` projectiles were spawned since the last Update;
  // their `age` holds the spawn age still to be simulated.
  int fresh_count_ = 0;
};

}  // namespace blue::weapon

#endif  // BLUE_WEAPON_PROJECTILE_SYSTEM_H_
