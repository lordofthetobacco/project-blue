#include "weapon/weapon_animator.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "animation/clip.h"
#include "animation/hierarchy.h"

namespace blue::weapon {
namespace {

// Translation differences below this (model units) count as no motion.
constexpr float kMotionEpsilon = 1.0e-5f;

// Returns component `axis` of key `key` of a translation channel.
float KeyValue(const animation::Channel& channel, size_t key, size_t axis) {
  constexpr size_t kComponents = 3;
  const size_t element =
      channel.interpolation == animation::Interpolation::kCubicSpline
          ? 3 * key + 1  // skip the in-tangent
          : key;
  return channel.values[element * kComponents + axis];
}

}  // namespace

FiringKeyTimes MeasureFiringKeys(const animation::Clip& clip, int node) {
  FiringKeyTimes result;
  for (const animation::Channel& channel : clip.channels) {
    if (channel.node != node ||
        channel.path != animation::TargetPath::kTranslation) {
      continue;
    }
    for (size_t key = 1; key < channel.times.size(); ++key) {
      size_t axis = 0;
      float largest = 0.0f;
      for (size_t i = 0; i < 3; ++i) {
        const float change =
            KeyValue(channel, key, i) - KeyValue(channel, 0, i);
        if (std::abs(change) > std::abs(largest)) {
          largest = change;
          axis = i;
        }
      }
      if (std::abs(largest) <= kMotionEpsilon) {
        continue;
      }
      result.motion_start = channel.times[key - 1];
      // Furthest along the direction of the first motion, earliest key.
      const float sign = largest > 0.0f ? 1.0f : -1.0f;
      float best = sign * KeyValue(channel, key, axis);
      result.extreme = channel.times[key];
      for (size_t later = key + 1; later < channel.times.size(); ++later) {
        const float value = sign * KeyValue(channel, later, axis);
        if (value > best) {
          best = value;
          result.extreme = channel.times[later];
        }
      }
      return result;
    }
  }
  return result;
}

WeaponAnimator::WeaponAnimator(const animation::Hierarchy* hierarchy,
                               const animation::Clip* fire_clip,
                               const WeaponAnimatorTuning& tuning)
    : hierarchy_(hierarchy),
      fire_clip_(fire_clip),
      tuning_(tuning),
      slide_node_(animation::FindNode(*hierarchy, tuning.slide_node)),
      player_(fire_clip->duration),
      pose_(hierarchy->nodes.size()) {
  tuning_.slide_node = {};  // not owned: do not keep a view of it
  for (const animation::Channel& channel : fire_clip_->channels) {
    if (channel.node == slide_node_ && slide_node_ >= 0) {
      slide_clip_.channels.push_back(channel);
    }
  }
  slide_clip_.duration = fire_clip_->duration;
  animation::ResetToRest(*hierarchy_, pose_);
}

void WeaponAnimator::Update(int shots, bool bolt_locked_back, float dt) {
  if (shots > 0) {
    // The shot broke somewhere in this frame: show the break pose now.
    player_.Play(tuning_.fire_time);
  } else {
    player_.Update(std::max(dt, 0.0f));
  }
  animation::ResetToRest(*hierarchy_, pose_);
  if (player_.playing()) {
    animation::SampleClip(*fire_clip_, player_.time(), pose_);
  }
  if (bolt_locked_back) {
    // Let a shot that just started the clip carry the slide back, then hold.
    const bool travelling =
        player_.playing() && player_.time() < tuning_.lock_time;
    animation::SampleClip(
        slide_clip_, travelling ? player_.time() : tuning_.lock_time, pose_);
  }
}

void WeaponAnimator::Reset() {
  player_ = animation::ClipPlayer(fire_clip_->duration);
  animation::ResetToRest(*hierarchy_, pose_);
}

void WeaponAnimator::ComputeWorld(const glm::mat4& root,
                                  std::span<glm::mat4> world) const {
  animation::ComputeWorldMatrices(*hierarchy_, pose_, world);
  const size_t count = std::min(world.size(), pose_.size());
  for (size_t i = 0; i < count; ++i) {
    world[i] = root * world[i];
  }
}

}  // namespace blue::weapon
