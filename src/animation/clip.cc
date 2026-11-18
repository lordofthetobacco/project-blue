#include "animation/clip.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <string_view>
#include <utility>

#include <glm/ext/quaternion_common.hpp>
#include <glm/ext/quaternion_geometric.hpp>

namespace blue::animation {
namespace {

// One element of a channel value (up to a quaternion).
using Element = std::array<float, 4>;

// The pair of keys bracketing a sample time and the position between them.
struct KeySpan {
  size_t first = 0;
  size_t second = 0;
  float fraction = 0.0f;  // in [0, 1]
  float interval = 0.0f;  // seconds between the two keys, 0 when clamped
};

// Returns the keys of `times` (non-empty, strictly increasing) around `time`,
// clamping before the first and after the last key. Binary search.
KeySpan FindKeys(const std::vector<float>& times, float time) {
  const size_t last = times.size() - 1;
  if (time <= times.front()) {
    return {};
  }
  if (time >= times.back()) {
    return {.first = last, .second = last};
  }
  const auto upper = std::upper_bound(times.begin(), times.end(), time);
  const auto second = static_cast<size_t>(upper - times.begin());
  const size_t first = second - 1;
  const float interval = times[second] - times[first];
  return {.first = first,
          .second = second,
          .fraction = (time - times[first]) / interval,
          .interval = interval};
}

// Returns element `slot` (0 in-tangent, 1 value, 2 out-tangent for cubic
// channels; always 0 otherwise) of key `key`.
Element ReadElement(const Channel& channel, size_t key, size_t slot) {
  const auto count = static_cast<size_t>(ComponentCount(channel.path));
  const size_t stride =
      channel.interpolation == Interpolation::kCubicSpline ? 3 : 1;
  const size_t offset = (key * stride + slot) * count;
  Element element{};
  std::copy_n(channel.values.begin() + static_cast<ptrdiff_t>(offset), count,
              element.begin());
  return element;
}

// Returns the stored value of `key`.
Element KeyValue(const Channel& channel, size_t key) {
  const size_t slot =
      channel.interpolation == Interpolation::kCubicSpline ? 1 : 0;
  return ReadElement(channel, key, slot);
}

glm::quat ToQuat(const Element& e) { return glm::quat(e[3], e[0], e[1], e[2]); }

Element FromQuat(const glm::quat& q) { return {q.x, q.y, q.z, q.w}; }

// Interpolates linearly between two keys; rotations slerp along the shortest
// path (glm::slerp flips the sign of the second quaternion if needed).
Element Linear(const Channel& channel, const KeySpan& keys) {
  const Element a = KeyValue(channel, keys.first);
  const Element b = KeyValue(channel, keys.second);
  if (channel.path == TargetPath::kRotation) {
    return FromQuat(glm::slerp(ToQuat(a), ToQuat(b), keys.fraction));
  }
  Element result{};
  for (size_t i = 0; i < 3; ++i) {
    result[i] = a[i] + (b[i] - a[i]) * keys.fraction;
  }
  return result;
}

// Evaluates the glTF cubic Hermite spline between two keys. Tangents are per
// second, so they are scaled by the key interval.
Element CubicSpline(const Channel& channel, const KeySpan& keys) {
  const Element p0 = ReadElement(channel, keys.first, 1);
  const Element m0 = ReadElement(channel, keys.first, 2);
  const Element p1 = ReadElement(channel, keys.second, 1);
  const Element m1 = ReadElement(channel, keys.second, 0);
  const float t = keys.fraction;
  const float t2 = t * t;
  const float t3 = t2 * t;
  const float h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
  const float h10 = (t3 - 2.0f * t2 + t) * keys.interval;
  const float h01 = -2.0f * t3 + 3.0f * t2;
  const float h11 = (t3 - t2) * keys.interval;
  const auto count = static_cast<size_t>(ComponentCount(channel.path));
  Element result{};
  for (size_t i = 0; i < count; ++i) {
    result[i] = h00 * p0[i] + h10 * m0[i] + h01 * p1[i] + h11 * m1[i];
  }
  return result;
}

// Returns the value of `channel` at `time`.
Element Evaluate(const Channel& channel, float time) {
  const KeySpan keys = FindKeys(channel.times, time);
  if (keys.first == keys.second) {
    return KeyValue(channel, keys.first);
  }
  switch (channel.interpolation) {
    case Interpolation::kStep:
      return KeyValue(channel, keys.first);
    case Interpolation::kLinear:
      return Linear(channel, keys);
    case Interpolation::kCubicSpline:
      return CubicSpline(channel, keys);
  }
  return KeyValue(channel, keys.first);
}

}  // namespace

int ComponentCount(TargetPath path) {
  return path == TargetPath::kRotation ? 4 : 3;
}

bool IsValid(const Channel& channel) {
  if (channel.times.empty() || channel.node < 0) {
    return false;
  }
  const size_t stride =
      channel.interpolation == Interpolation::kCubicSpline ? 3 : 1;
  const size_t expected = channel.times.size() * stride *
                          static_cast<size_t>(ComponentCount(channel.path));
  if (channel.values.size() != expected) {
    return false;
  }
  return std::adjacent_find(channel.times.begin(), channel.times.end(),
                            [](float a, float b) { return b <= a; }) ==
         channel.times.end();
}

void SampleClip(const Clip& clip, float time, std::span<Transform> pose) {
  for (const Channel& channel : clip.channels) {
    if (channel.node < 0 || static_cast<size_t>(channel.node) >= pose.size()) {
      continue;
    }
    const Element value = Evaluate(channel, time);
    Transform& target = pose[static_cast<size_t>(channel.node)];
    switch (channel.path) {
      case TargetPath::kTranslation:
        target.translation = {value[0], value[1], value[2]};
        break;
      case TargetPath::kRotation:
        target.rotation = glm::normalize(ToQuat(value));
        break;
      case TargetPath::kScale:
        target.scale = {value[0], value[1], value[2]};
        break;
    }
  }
}

Clip MergeClips(std::span<const Clip> clips, std::string name) {
  Clip merged{.name = std::move(name)};
  size_t channel_count = 0;
  size_t event_count = 0;
  for (const Clip& clip : clips) {
    channel_count += clip.channels.size();
    event_count += clip.events.size();
  }
  merged.channels.reserve(channel_count);
  merged.events.reserve(event_count);
  merged.loop = !clips.empty();
  for (const Clip& clip : clips) {
    merged.duration = std::max(merged.duration, clip.duration);
    merged.channels.insert(merged.channels.end(), clip.channels.begin(),
                           clip.channels.end());
    merged.events.insert(merged.events.end(), clip.events.begin(),
                         clip.events.end());
    merged.loop = merged.loop && clip.loop;
  }
  std::ranges::stable_sort(merged.events, {}, &ClipEvent::time);
  return merged;
}

void AddEvent(Clip& clip, ClipEvent event) {
  const auto position =
      std::ranges::upper_bound(clip.events, event.time, {}, &ClipEvent::time);
  clip.events.insert(position, std::move(event));
}

const ClipEvent* FindEvent(const Clip& clip, std::string_view name) {
  const auto found = std::ranges::find(clip.events, name, &ClipEvent::name);
  return found == clip.events.end() ? nullptr : &*found;
}

bool IsValid(const Clip& clip) {
  if (!std::isfinite(clip.duration) || clip.duration < 0.0f) {
    return false;
  }
  const auto valid_channel = [](const Channel& channel) {
    return IsValid(channel);
  };
  if (!std::ranges::all_of(clip.channels, valid_channel)) {
    return false;
  }
  const auto finite_time = [](const ClipEvent& event) {
    return std::isfinite(event.time);
  };
  return std::ranges::all_of(clip.events, finite_time) &&
         std::ranges::is_sorted(clip.events, {}, &ClipEvent::time);
}

}  // namespace blue::animation
