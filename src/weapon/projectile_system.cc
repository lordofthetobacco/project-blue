#include "weapon/projectile_system.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>

#include <glm/geometric.hpp>

namespace blue::weapon {
namespace {

constexpr float kMinSubstep = 1.0e-4f;  // s, guards a degenerate tuning
constexpr int kMaxSubsteps = 1024;      // per bullet per Update
// Segments reserved per bullet of capacity: a 1/30 s frame at the default
// 1/240 s substep.
constexpr size_t kReservedSubsteps = 8;

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
  const auto capacity = static_cast<size_t>(tuning_.capacity);
  // At most one impact per bullet and Update.
  projectiles_.reserve(capacity);
  impacts_.reserve(capacity);
  player_impacts_.reserve(capacity);
  flights_.reserve(capacity);
  segments_.reserve(capacity * kReservedSubsteps);
  tracks_.reserve(capacity * kReservedSubsteps);
}

void ProjectileSystem::Spawn(glm::vec3 position, glm::vec3 velocity, float mass,
                             float drag, float age, int shot_number,
                             uint32_t owner) {
  if (tuning_.capacity == 0) {
    return;
  }
  DiscardSweep();
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
      .owner = owner,
      .id = next_id_,
  });
  next_id_ = next_id_ == std::numeric_limits<int>::max() ? 1 : next_id_ + 1;
  ++fresh_count_;
}

void ProjectileSystem::Update(physics::PhysicsWorld& world, glm::vec3 gravity,
                              float dt) {
  Sweep(world, gravity, dt);
  Resolve(world, {});
}

void ProjectileSystem::Sweep(const physics::PhysicsWorld& world,
                             glm::vec3 gravity, float dt) {
  dt = std::max(dt, 0.0f);
  segments_.clear();
  tracks_.clear();
  flights_.clear();
  const size_t first_fresh =
      projectiles_.size() - static_cast<size_t>(fresh_count_);
  for (size_t i = 0; i < projectiles_.size(); ++i) {
    Projectile bullet = projectiles_[i];
    float seconds = dt;
    if (i >= first_fresh) {
      seconds = bullet.age;
      bullet.age = 0.0f;
    }
    bullet.previous_position = bullet.position;
    flights_.push_back(
        SweepBullet(world, gravity, seconds, static_cast<int>(i), bullet));
  }
  swept_ = true;
}

void ProjectileSystem::Resolve(physics::PhysicsWorld& world,
                               std::span<const PlayerHit> player_hits) {
  impacts_.clear();
  player_impacts_.clear();
  if (!swept_) {
    return;
  }
  // The earliest valid player hit of each bullet: segments of one bullet are
  // consecutive in flight order, so a lower index is earlier.
  for (const PlayerHit& hit : player_hits) {
    if (hit.segment < 0 ||
        static_cast<size_t>(hit.segment) >= segments_.size()) {
      continue;
    }
    const auto segment_index = static_cast<size_t>(hit.segment);
    const Segment& segment = segments_[segment_index];
    const float fraction = std::clamp(hit.fraction, 0.0f, 1.0f);
    if (!(fraction < segment.world_fraction)) {
      continue;  // behind the world hit (or NaN)
    }
    Flight& flight =
        flights_[static_cast<size_t>(tracks_[segment_index].bullet)];
    const bool earlier = flight.player_segment < 0 ||
                         hit.segment < flight.player_segment ||
                         (hit.segment == flight.player_segment &&
                          fraction < flight.player_fraction);
    if (earlier) {
      flight.player_segment = hit.segment;
      flight.player_fraction = fraction;
      flight.victim = hit.victim;
    }
  }

  size_t kept = 0;
  for (size_t i = 0; i < projectiles_.size(); ++i) {
    const Flight& flight = flights_[i];
    if (flight.player_segment >= 0) {
      const auto segment_index = static_cast<size_t>(flight.player_segment);
      const Segment& segment = segments_[segment_index];
      const SegmentTrack& track = tracks_[segment_index];
      const float fraction = flight.player_fraction;
      player_impacts_.push_back(PlayerImpact{
          .victim = flight.victim,
          .point = segment.origin + fraction * segment.delta,
          .velocity = segment.velocity +
                      fraction * (track.end_velocity - segment.velocity),
          .mass = segment.mass,
          .shot_number = segment.shot_number,
          .owner = segment.owner,
          .flight_time = track.start_age + fraction * track.duration,
          .distance = track.start_distance + fraction * track.length,
      });
      continue;
    }
    if (flight.world_hit) {
      // The bullet's last segment holds the hit.
      const auto last = static_cast<size_t>(flight.last_segment);
      const Segment& segment = segments_[last];
      const SegmentTrack& track = tracks_[last];
      const float fraction = segment.world_fraction;
      const glm::vec3 impact_velocity =
          segment.velocity + fraction * (track.end_velocity - segment.velocity);
      impacts_.push_back(Impact{
          .point = flight.end.position,
          .normal = segment.world_normal,
          .velocity = impact_velocity,
          .mass = segment.mass,
          .body = segment.world_body,
          .flight_time = track.start_age + fraction * track.duration,
          .distance = track.start_distance + fraction * track.length,
          .shot_number = segment.shot_number,
      });
      if (world.IsDynamic(segment.world_body)) {
        world.AddImpulseAtPoint(segment.world_body,
                                segment.mass * impact_velocity,
                                flight.end.position);
      }
      continue;
    }
    if (flight.alive) {
      projectiles_[kept++] = flight.end;
    }
  }
  projectiles_.resize(kept);
  fresh_count_ = 0;
  DiscardSweep();
}

void ProjectileSystem::Clear() {
  projectiles_.clear();
  impacts_.clear();
  player_impacts_.clear();
  DiscardSweep();
  fresh_count_ = 0;
}

void ProjectileSystem::DiscardSweep() {
  segments_.clear();
  tracks_.clear();
  flights_.clear();
  swept_ = false;
}

ProjectileSystem::Flight ProjectileSystem::SweepBullet(
    const physics::PhysicsWorld& world, glm::vec3 gravity, float seconds,
    int index, Projectile bullet) {
  Flight flight;
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
          world.CastRay(bullet.position, segment, length,
                        tuning_.hits_cosmetic ? physics::RayFilter::kEverything
                                              : physics::RayFilter::kGameplay);
      segments_.push_back(Segment{
          .projectile = bullet.id,
          .shot_number = bullet.shot_number,
          .owner = bullet.owner,
          .origin = bullet.position,
          .delta = segment,
          .world_fraction = hit.has_value() ? hit->fraction : 1.0f,
          .velocity = velocity,
          .mass = bullet.mass,
          .world_body = hit.has_value() ? hit->body : physics::BodyId{},
          .world_normal = hit.has_value() ? hit->normal : glm::vec3(0.0f),
      });
      tracks_.push_back(SegmentTrack{
          .bullet = index,
          .end_velocity = next_velocity,
          .start_age = bullet.age,
          .duration = h,
          .start_distance = bullet.distance,
          .length = length,
      });
      if (hit.has_value()) {
        bullet.position = hit->point;
        flight.end = bullet;
        flight.world_hit = true;
        flight.last_segment = static_cast<int>(segments_.size()) - 1;
        return flight;
      }
      bullet.position += segment;
      bullet.velocity = next_velocity;
      bullet.age += h;
      bullet.distance += length;
      if (bullet.age >= tuning_.max_age ||
          bullet.distance >= tuning_.max_distance) {
        flight.end = bullet;
        return flight;
      }
    }
  }
  flight.alive =
      std::isfinite(bullet.position.x) && std::isfinite(bullet.position.y) &&
      std::isfinite(bullet.position.z) && bullet.age < tuning_.max_age &&
      bullet.distance < tuning_.max_distance;
  flight.end = bullet;
  return flight;
}

}  // namespace blue::weapon
