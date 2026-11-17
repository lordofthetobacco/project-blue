#ifndef BLUE_WEAPON_TARGET_LOG_H_
#define BLUE_WEAPON_TARGET_LOG_H_

#include <vector>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include "weapon/projectile_system.h"

namespace blue::weapon {

// A flat target plate. World space, metres.
struct Target {
  glm::vec3 center{0.0f};
  glm::vec3 normal{0.0f, 0.0f, 1.0f};  // unit, facing the shooter
  glm::vec3 up{0.0f, 1.0f, 0.0f};      // need not be exactly in the plane
  glm::vec2 half_extents{0.25f};       // (right, up) half width and height
  float range_m = 0.0f;                // shooter-to-target distance
};

// One recorded hit on a target.
struct TargetHit {
  int target = 0;             // index given to Record
  float range_m = 0.0f;       // the target's range_m
  glm::vec2 offset_m{0.0f};   // (right, up) of the target centre, in its plane
  float miss_m = 0.0f;        // |offset_m|
  bool on_plate = false;      // the offset lies within the half extents
  float flight_time = 0.0f;   // s
  float impact_speed = 0.0f;  // m/s
  float energy_j = 0.0f;      // kinetic energy at the hit
  int shot_number = 0;        // Impact::shot_number
};

// Fixed-capacity ring log of hits on a fixed number of targets: the newest
// hits replace the oldest once full, per-target counts keep counting. Right
// and up are as seen by a shooter facing the target (looking along -normal).
// No allocation after construction. Copyable. Not thread-safe.
class TargetLog {
 public:
  // Holds the last `capacity` hits (at least 1) on targets 0 .. target_count
  // - 1 (at least 0).
  explicit TargetLog(int capacity = 64, int target_count = 16);
  TargetLog(const TargetLog&) = default;
  TargetLog& operator=(const TargetLog&) = default;

  // Records `impact` on `target`, which has index `target_index`. Returns
  // false, recording nothing, for an index outside [0, target_count).
  bool Record(int target_index, const Target& target, const Impact& impact);

  // Hits currently held, at most capacity().
  [[nodiscard]] int size() const { return size_; }
  [[nodiscard]] int capacity() const { return static_cast<int>(hits_.size()); }
  // Returns hit `i` counting from the newest (0) to the oldest (size() - 1).
  // `i` must be in [0, size()).
  [[nodiscard]] const TargetHit& hit(int i) const;
  // Hits recorded on `target_index` since the last Clear (0 when out of
  // range), including those that have left the ring.
  [[nodiscard]] int hits_on(int target_index) const;
  [[nodiscard]] int target_count() const {
    return static_cast<int>(hit_counts_.size());
  }
  // All hits recorded since the last Clear.
  [[nodiscard]] int total_hits() const { return total_hits_; }

  // Forgets every hit and count.
  void Clear();

 private:
  std::vector<TargetHit> hits_;  // ring storage, size == capacity
  std::vector<int> hit_counts_;  // per target
  int next_ = 0;                 // slot the next hit goes to
  int size_ = 0;
  int total_hits_ = 0;
};

}  // namespace blue::weapon

#endif  // BLUE_WEAPON_TARGET_LOG_H_
