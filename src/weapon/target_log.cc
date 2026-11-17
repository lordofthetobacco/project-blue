#include "weapon/target_log.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

#include <glm/geometric.hpp>

#include "core/log.h"

namespace blue::weapon {
namespace {

constexpr float kMinLength = 1.0e-6f;

// Returns the unit right and up vectors of `target`'s plane as seen by a
// shooter looking along -normal. Falls back to world axes when `up` is
// (nearly) parallel to the normal.
void PlaneAxes(const Target& target, glm::vec3& right, glm::vec3& up) {
  const glm::vec3 normal = glm::length(target.normal) > kMinLength
                               ? glm::normalize(target.normal)
                               : glm::vec3(0.0f, 0.0f, 1.0f);
  glm::vec3 in_plane = target.up - glm::dot(target.up, normal) * normal;
  if (glm::length(in_plane) < kMinLength) {
    const glm::vec3 fallback = std::abs(normal.y) < 0.99f
                                   ? glm::vec3(0.0f, 1.0f, 0.0f)
                                   : glm::vec3(0.0f, 0.0f, -1.0f);
    in_plane = fallback - glm::dot(fallback, normal) * normal;
  }
  up = glm::normalize(in_plane);
  right = glm::cross(up, normal);
}

}  // namespace

TargetLog::TargetLog(int capacity, int target_count)
    : hits_(static_cast<size_t>(std::max(capacity, 1))),
      hit_counts_(static_cast<size_t>(std::max(target_count, 0)), 0) {}

bool TargetLog::Record(int target_index, const Target& target,
                       const Impact& impact) {
  if (target_index < 0 || target_index >= target_count()) {
    return false;
  }
  glm::vec3 right;
  glm::vec3 up;
  PlaneAxes(target, right, up);
  const glm::vec3 relative = impact.point - target.center;
  const glm::vec2 offset(glm::dot(relative, right), glm::dot(relative, up));
  const float speed = glm::length(impact.velocity);
  hits_[static_cast<size_t>(next_)] = TargetHit{
      .target = target_index,
      .range_m = target.range_m,
      .offset_m = offset,
      .miss_m = glm::length(offset),
      .on_plate = std::abs(offset.x) <= target.half_extents.x &&
                  std::abs(offset.y) <= target.half_extents.y,
      .flight_time = impact.flight_time,
      .impact_speed = speed,
      .energy_j = 0.5f * impact.mass * speed * speed,
      .shot_number = impact.shot_number,
  };
  next_ = (next_ + 1) % capacity();
  size_ = std::min(size_ + 1, capacity());
  ++hit_counts_[static_cast<size_t>(target_index)];
  ++total_hits_;
  return true;
}

const TargetHit& TargetLog::hit(int i) const {
  if (i < 0 || i >= size_) {
    core::log::Fatal("TargetLog::hit: index {} outside [0, {})", i, size_);
  }
  const int slot = ((next_ - 1 - i) % capacity() + capacity()) % capacity();
  return hits_[static_cast<size_t>(slot)];
}

int TargetLog::hits_on(int target_index) const {
  if (target_index < 0 || target_index >= target_count()) {
    return 0;
  }
  return hit_counts_[static_cast<size_t>(target_index)];
}

void TargetLog::Clear() {
  std::fill(hit_counts_.begin(), hit_counts_.end(), 0);
  next_ = 0;
  size_ = 0;
  total_hits_ = 0;
}

}  // namespace blue::weapon
