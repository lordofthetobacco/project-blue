#include "sim/damage.h"

#include <cmath>
#include <cstdint>
#include <optional>
#include <span>

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

#include "core/det_math.h"
#include "physics/physics_world.h"
#include "scene/scene_data.h"
#include "sim/player_sim.h"

namespace blue::sim {
namespace {

constexpr glm::vec3 kDown{0.0f, -1.0f, 0.0f};
constexpr glm::vec3 kUp{0.0f, 1.0f, 0.0f};
// m above the floor point the clearance ray starts, so it does not start in
// the floor itself.
constexpr float kClearanceLift = 0.05f;
// 2 pi rounded to float.
constexpr float kTurn = 6.2831853f;

// Returns the floor point below `position` within `distance`, or nullopt.
std::optional<glm::vec3> FloorBelow(const physics::PhysicsWorld& world,
                                    glm::vec3 position, float distance) {
  const std::optional<physics::RayHit> hit =
      world.CastRay(position, kDown, distance);
  if (!hit) {
    return std::nullopt;
  }
  return hit->point;
}

bool TooClose(glm::vec3 candidate, std::span<const glm::vec3> occupied,
              float separation) {
  for (const glm::vec3& other : occupied) {
    const float dx = candidate.x - other.x;
    const float dz = candidate.z - other.z;
    if (dx * dx + dz * dz < separation * separation) {
      return true;
    }
  }
  return false;
}

// Returns the feet position of ring candidate `index`, or nullopt when it
// cannot be used.
std::optional<glm::vec3> TryCandidate(const physics::PhysicsWorld& world,
                                      glm::vec3 center, int index,
                                      std::span<const glm::vec3> occupied,
                                      const SpawnRing& ring) {
  const float angle =
      kTurn * static_cast<float>(index) / static_cast<float>(ring.points);
  const core::SinCos around = core::DetSinCos(angle);
  const glm::vec3 offset(around.sin * ring.radius, 0.0f,
                         -around.cos * ring.radius);
  if (world.CastRay(center, offset, ring.radius)) {
    return std::nullopt;  // A wall between the start and the candidate.
  }
  const glm::vec3 above = center + offset;
  const std::optional<glm::vec3> floor =
      FloorBelow(world, above, ring.probe_distance);
  if (!floor || TooClose(*floor, occupied, ring.min_separation)) {
    return std::nullopt;
  }
  if (world.CastRay(*floor + kUp * kClearanceLift, kUp,
                    ring.clearance_height - kClearanceLift)) {
    return std::nullopt;
  }
  return *floor;
}

}  // namespace

HitZone ZoneOfHeight(float height_fraction) {
  if (std::isnan(height_fraction)) {
    return HitZone::kTorso;
  }
  if (height_fraction >= 1.0f - kHeadShare) {
    return HitZone::kHead;
  }
  if (height_fraction < kLegShare) {
    return HitZone::kLegs;
  }
  return HitZone::kTorso;
}

float ZoneMultiplier(HitZone zone) {
  switch (zone) {
    case HitZone::kHead:
      return 4.0f;
    case HitZone::kTorso:
      return 1.0f;
    case HitZone::kLegs:
      return 0.6f;
  }
  return 1.0f;
}

int ComputeDamage(float mass, glm::vec3 bullet_velocity,
                  glm::vec3 target_velocity, HitZone zone) {
  const float speed = glm::length(bullet_velocity - target_velocity);
  const float damage = kDamagePerMomentum * mass * speed * ZoneMultiplier(zone);
  if (!std::isfinite(damage) || !(damage > 0.0f)) {
    return 0;
  }
  // Large enough to kill anyone, small enough to stay an int.
  constexpr float kMaxDamage = 1.0e6f;
  const auto rounded = static_cast<int>(std::lround(
      static_cast<double>(damage < kMaxDamage ? damage : kMaxDamage)));
  return rounded > 0 ? rounded : 1;
}

glm::vec3 ChooseSpawnPoint(const physics::PhysicsWorld& world,
                           const scene::PlayerStart& start, uint32_t player_id,
                           uint32_t deaths, std::span<const glm::vec3> occupied,
                           float fallback_eye_height, const SpawnRing& ring) {
  if (ring.points > 0) {
    const auto first = static_cast<int>(ShotSeed(player_id, deaths) %
                                        static_cast<uint32_t>(ring.points));
    for (int i = 0; i < ring.points; ++i) {
      if (const std::optional<glm::vec3> feet =
              TryCandidate(world, start.position, (first + i) % ring.points,
                           occupied, ring)) {
        return *feet;
      }
    }
  }
  if (const std::optional<glm::vec3> floor =
          FloorBelow(world, start.position, ring.probe_distance)) {
    return *floor;
  }
  return start.position - kUp * fallback_eye_height;
}

}  // namespace blue::sim
