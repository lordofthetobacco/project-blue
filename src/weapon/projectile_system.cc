#include "weapon/projectile_system.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>

#include <glm/geometric.hpp>

namespace blue::weapon {
namespace {

constexpr float kMinSubstep = 1.0e-4f;  // s, guards a degenerate tuning
constexpr int kMaxSubsteps = 1024;      // per bullet per Update

// Returns the acceleration of a bullet moving with `velocity`.
glm::vec3 Acceleration(glm::vec3 gravity, float drag, glm::vec3 velocity) {
  return gravity - drag * glm::length(velocity) * velocity;
}

}  // namespace

BallisticStep IntegrateBallisticStep(glm::vec3 velocity, glm::vec3 gravity,
                                     float drag, float h) {
  const glm::vec3 mid_velocity =
      velocity + 0.5f * h * Acceleration(gravity, drag, velocity);
  return {.displacement = mid_velocity * h,
          .velocity = velocity + h * Acceleration(gravity, drag, mid_velocity)};
}

ProjectileSystem::ProjectileSystem(const ProjectileTuning& tuning)
    : tuning_(tuning) {
  tuning_.capacity = std::max(tuning_.capacity, 0);
  tuning_.max_substep = std::max(tuning_.max_substep, kMinSubstep);
  // At most one impact per bullet and Update.
  projectiles_.reserve(static_cast<size_t>(tuning_.capacity));
  impacts_.reserve(static_cast<size_t>(tuning_.capacity));
}

void ProjectileSystem::Spawn(glm::vec3 position, glm::vec3 velocity, float mass,
                             float drag, float age, int shot_number) {
  if (tuning_.capacity == 0) {
    return;
  }
  if (projectiles_.size() >= static_cast<size_t>(tuning_.capacity)) {
    projectiles_.erase(projectiles_.begin());
    fresh_count_ =
        std::min(fresh_count_, static_cast<int>(projectiles_.size()));
  }
  projectiles_.push_back(Projectile{
      .position = position,
      .previous_position = position,
      .velocity = velocity,
      .mass = mass,
      .drag = std::max(drag, 0.0f),
      .age = std::max(age, 0.0f),
      .distance = 0.0f,
      .shot_number = shot_number,
  });
  ++fresh_count_;
}

void ProjectileSystem::Update(physics::PhysicsWorld& world, glm::vec3 gravity,
                              float dt) {
  dt = std::max(dt, 0.0f);
  impacts_.clear();
  const size_t first_fresh =
      projectiles_.size() - static_cast<size_t>(fresh_count_);
  size_t kept = 0;
  for (size_t i = 0; i < projectiles_.size(); ++i) {
    Projectile bullet = projectiles_[i];
    float seconds = dt;
    if (i >= first_fresh) {
      seconds = bullet.age;
      bullet.age = 0.0f;
    }
    bullet.previous_position = bullet.position;
    if (Advance(world, gravity, seconds, bullet)) {
      projectiles_[kept++] = bullet;
    }
  }
  projectiles_.resize(kept);
  fresh_count_ = 0;
}

void ProjectileSystem::Clear() {
  projectiles_.clear();
  impacts_.clear();
  fresh_count_ = 0;
}

bool ProjectileSystem::Advance(physics::PhysicsWorld& world, glm::vec3 gravity,
                               float seconds, Projectile& bullet) {
  if (seconds > 0.0f) {
    const int substeps = std::clamp(
        // The tolerance keeps e.g. 1/30 s at 8 substeps of 1/240 s despite
        // rounding, so equal frame times always split identically.
        static_cast<int>(std::ceil(seconds / tuning_.max_substep - 1.0e-3f)), 1,
        kMaxSubsteps);
    const float h = seconds / static_cast<float>(substeps);
    for (int step = 0; step < substeps; ++step) {
      const glm::vec3 velocity = bullet.velocity;
      const BallisticStep next =
          IntegrateBallisticStep(velocity, gravity, bullet.drag, h);
      const glm::vec3 next_velocity = next.velocity;
      const glm::vec3 segment = next.displacement;
      const float length = glm::length(segment);
      const std::optional<physics::RayHit> hit =
          world.CastRay(bullet.position, segment, length);
      if (hit.has_value()) {
        const glm::vec3 impact_velocity =
            velocity + hit->fraction * (next_velocity - velocity);
        impacts_.push_back(Impact{
            .point = hit->point,
            .normal = hit->normal,
            .velocity = impact_velocity,
            .mass = bullet.mass,
            .body = hit->body,
            .flight_time = bullet.age + hit->fraction * h,
            .distance = bullet.distance + hit->fraction * length,
            .shot_number = bullet.shot_number,
        });
        if (world.IsDynamic(hit->body)) {
          world.AddImpulseAtPoint(hit->body, bullet.mass * impact_velocity,
                                  hit->point);
        }
        bullet.position = hit->point;
        return false;
      }
      bullet.position += segment;
      bullet.velocity = next_velocity;
      bullet.age += h;
      bullet.distance += length;
      if (bullet.age >= tuning_.max_age ||
          bullet.distance >= tuning_.max_distance) {
        return false;
      }
    }
  }
  return std::isfinite(bullet.position.x) && std::isfinite(bullet.position.y) &&
         std::isfinite(bullet.position.z) && bullet.age < tuning_.max_age &&
         bullet.distance < tuning_.max_distance;
}

}  // namespace blue::weapon
