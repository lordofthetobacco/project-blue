#include "interact/interaction_focus.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <span>
#include <utility>

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/mat3x3.hpp>
#include <glm/matrix.hpp>
#include <glm/vec3.hpp>

namespace blue::interact {
namespace {

// Golden-section steps for the closest pass of a ray that misses: shrinks
// the search interval by 0.618 per step, to well under a micrometre.
constexpr int kClosestPassSteps = 48;
constexpr float kInverseGoldenRatio = 0.618034f;
// Directions shorter than this focus nothing.
constexpr float kMinDirectionLength = 1.0e-6f;
// Angles closer than this count as equal (both rays hit, angle 0).
constexpr float kAngleTie = 1.0e-5f;

bool IsFinite(glm::vec3 v) {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// The box in its own frame: `point` in box space is axes^T (point - center).
struct LocalBox {
  const OrientedBox* box;
  glm::mat3 to_local;
  glm::vec3 half;

  explicit LocalBox(const OrientedBox& b)
      : box(&b),
        to_local(glm::transpose(b.axes)),
        half(glm::abs(b.half_extents)) {}

  [[nodiscard]] glm::vec3 ToLocal(glm::vec3 point) const {
    return to_local * (point - box->center);
  }
  [[nodiscard]] glm::vec3 ToWorld(glm::vec3 local) const {
    return box->center + box->axes * local;
  }
  // Distance from the box-space `local` point to the box.
  [[nodiscard]] float Distance(glm::vec3 local) const {
    return glm::length(local - glm::clamp(local, -half, half));
  }
};

// Returns where the ray (box space) enters the box, 0 when it starts
// inside, or nullopt when it misses.
std::optional<float> EnterDistance(glm::vec3 origin, glm::vec3 direction,
                                   glm::vec3 half) {
  float enter = 0.0f;
  float leave = std::numeric_limits<float>::infinity();
  for (int axis = 0; axis < 3; ++axis) {
    const float o = origin[axis];
    const float d = direction[axis];
    const float h = half[axis];
    if (std::abs(d) < kMinDirectionLength) {
      if (o < -h || o > h) {
        return std::nullopt;
      }
      continue;
    }
    float t0 = (-h - o) / d;
    float t1 = (h - o) / d;
    if (t0 > t1) {
      std::swap(t0, t1);
    }
    enter = std::max(enter, t0);
    leave = std::min(leave, t1);
    if (enter > leave) {
      return std::nullopt;
    }
  }
  return enter;
}

// The current focus's limits are widened by the release tuning.
bool IsFocusable(const AimMeasure& aim, const FocusTuning& tuning,
                 bool focused) {
  const float reach = tuning.reach + (focused ? tuning.release_reach : 0.0f);
  const float scale = focused ? tuning.release_aim_scale : 1.0f;
  return aim.reach_distance <= reach &&
         aim.miss_distance <= tuning.aim_radius * scale &&
         aim.miss_angle <= tuning.max_aim_angle * scale;
}

// Whether `a` is preferred over `b`, before hysteresis.
bool Better(const AimMeasure& a, const AimMeasure& b) {
  if (std::abs(a.miss_angle - b.miss_angle) > kAngleTie) {
    return a.miss_angle < b.miss_angle;
  }
  return a.along < b.along;
}

// Whether `challenger` is clearly better than the `focused` shape.
bool ClearlyBetter(const AimMeasure& challenger, const AimMeasure& focused,
                   const FocusTuning& tuning) {
  if (challenger.miss_angle + tuning.switch_angle < focused.miss_angle) {
    return true;
  }
  const bool both_straight_on =
      challenger.miss_angle <= kAngleTie && focused.miss_angle <= kAngleTie;
  return both_straight_on &&
         challenger.along + tuning.switch_distance < focused.along;
}

}  // namespace

AimMeasure MeasureAim(glm::vec3 eye, glm::vec3 direction,
                      const OrientedBox& box) {
  const LocalBox local(box);
  const glm::vec3 origin = local.ToLocal(eye);
  const glm::vec3 ray = local.to_local * direction;

  AimMeasure aim;
  aim.reach_distance = local.Distance(origin);
  if (const std::optional<float> enter =
          EnterDistance(origin, ray, local.half)) {
    aim.along = *enter;
    aim.aim_point = eye + direction * *enter;
    return aim;
  }
  // A miss: the distance to the box along the ray is convex, so a golden
  // section search finds its minimum. No box point projects onto the ray
  // beyond `limit`, where the distance only grows.
  const float limit = std::max(
      glm::dot(box.center - eye, direction) + glm::length(local.half), 0.0f);
  const auto distance_at = [&](float t) {
    return local.Distance(origin + ray * t);
  };
  float low = 0.0f;
  float high = limit;
  for (int step = 0; step < kClosestPassSteps; ++step) {
    const float span = (high - low) * kInverseGoldenRatio;
    const float a = high - span;
    const float b = low + span;
    if (distance_at(a) <= distance_at(b)) {
      high = b;
    } else {
      low = a;
    }
  }
  const float t = 0.5f * (low + high);
  const glm::vec3 on_ray = origin + ray * t;
  aim.along = t;
  aim.miss_distance = local.Distance(on_ray);
  aim.miss_angle = std::atan2(aim.miss_distance, t);
  aim.aim_point = local.ToWorld(glm::clamp(on_ray, -local.half, local.half));
  return aim;
}

std::span<const RankedCandidate> InteractionFocus::Rank(
    const FocusQuery& query, std::span<const Interactable> candidates) {
  ranked_.clear();
  const float length = glm::length(query.direction);
  if (!(length > kMinDirectionLength) || !IsFinite(query.direction) ||
      !IsFinite(query.eye)) {
    return {};
  }
  const glm::vec3 direction = query.direction / length;
  std::optional<size_t> focused;  // index into ranked_
  for (size_t i = 0; i < candidates.size(); ++i) {
    const bool is_focus = focus_.has_value() &&
                          focus_->id == candidates[i].id &&
                          focus_->kind == candidates[i].kind;
    const AimMeasure aim =
        MeasureAim(query.eye, direction, candidates[i].shape);
    if (!IsFocusable(aim, tuning_, is_focus)) {
      continue;
    }
    if (is_focus) {
      focused = ranked_.size();
    }
    ranked_.push_back({.index = i, .aim = aim});
  }
  if (ranked_.empty()) {
    return {};
  }
  // Hysteresis: the focus goes first unless a neighbour is clearly better.
  std::optional<RankedCandidate> keep;
  if (focused.has_value()) {
    keep = ranked_[*focused];
    ranked_.erase(ranked_.begin() + static_cast<std::ptrdiff_t>(*focused));
  }
  std::sort(ranked_.begin(), ranked_.end(),
            [](const RankedCandidate& a, const RankedCandidate& b) {
              return Better(a.aim, b.aim);
            });
  if (keep.has_value()) {
    const bool beaten = !ranked_.empty() &&
                        ClearlyBetter(ranked_.front().aim, keep->aim, tuning_);
    // Beaten, it goes back among the others in plain order.
    const auto position =
        beaten ? std::find_if(ranked_.begin() + 1, ranked_.end(),
                              [&](const RankedCandidate& other) {
                                return Better(keep->aim, other.aim);
                              })
               : ranked_.begin();
    ranked_.insert(position, *keep);
  }
  return ranked_;
}

}  // namespace blue::interact
