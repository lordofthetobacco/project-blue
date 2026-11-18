#include "cook/clip_tools.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "animation/clip.h"
#include "animation/transform.h"

namespace blue::cook {
namespace {

using animation::Channel;
using animation::Clip;
using animation::Interpolation;
using animation::TargetPath;

// Keys closer than this to a trim boundary count as lying on it.
constexpr float kTimeEpsilon = 1.0e-6f;

bool ValidIndex(const prefab::Prefab& prefab, int clip) {
  return clip >= 0 && static_cast<size_t>(clip) < prefab.clips.size();
}

// Returns the value of `channel` at `time` as ComponentCount(path) floats
// (rotations x, y, z, w), sampled with the channel's interpolation.
std::vector<float> SampleChannel(const Channel& channel, float time) {
  Clip single{.channels = {channel}};
  single.channels[0].node = 0;
  animation::Transform pose;
  animation::SampleClip(single, time, std::span(&pose, 1));
  switch (channel.path) {
    case TargetPath::kTranslation:
      return {pose.translation.x, pose.translation.y, pose.translation.z};
    case TargetPath::kRotation:
      return {pose.rotation.x, pose.rotation.y, pose.rotation.z,
              pose.rotation.w};
    case TargetPath::kScale:
      return {pose.scale.x, pose.scale.y, pose.scale.z};
  }
  return {};
}

// Appends one key at `time` holding `value` to `out`, with zero tangents for
// cubic-spline channels.
void AppendKey(Channel& out, float time, std::span<const float> value) {
  out.times.push_back(time);
  if (out.interpolation == Interpolation::kCubicSpline) {
    out.values.insert(out.values.end(), value.size(), 0.0f);
    out.values.insert(out.values.end(), value.begin(), value.end());
    out.values.insert(out.values.end(), value.size(), 0.0f);
  } else {
    out.values.insert(out.values.end(), value.begin(), value.end());
  }
}

// Returns `channel` cut to [start, end] and shifted to start at 0.
Channel TrimChannel(const Channel& channel, float start, float end) {
  const auto components =
      static_cast<size_t>(animation::ComponentCount(channel.path));
  const size_t elements_per_key =
      channel.interpolation == Interpolation::kCubicSpline ? 3 : 1;
  const size_t key_floats = components * elements_per_key;

  Channel out{.node = channel.node,
              .path = channel.path,
              .interpolation = channel.interpolation};
  const auto has_key_near = [&](float time) {
    return std::ranges::any_of(channel.times, [time](float t) {
      return std::abs(t - time) <= kTimeEpsilon;
    });
  };
  if (!has_key_near(start)) {
    AppendKey(out, 0.0f, SampleChannel(channel, start));
  }
  for (size_t k = 0; k < channel.times.size(); ++k) {
    const float time = channel.times[k];
    if (time < start - kTimeEpsilon || time > end + kTimeEpsilon) {
      continue;
    }
    out.times.push_back(std::clamp(time - start, 0.0f, end - start));
    const auto first =
        channel.values.begin() + static_cast<std::ptrdiff_t>(k * key_floats);
    out.values.insert(out.values.end(), first,
                      first + static_cast<std::ptrdiff_t>(key_floats));
  }
  if (!has_key_near(end)) {
    AppendKey(out, end - start, SampleChannel(channel, end));
  }
  return out;
}

}  // namespace

core::Result<void, CookError> RenameClip(prefab::Prefab& prefab, int clip,
                                         std::string name) {
  using Result = core::Result<void, CookError>;
  if (!ValidIndex(prefab, clip) || name.empty()) {
    return Result::Error(CookError::kInvalidArgument);
  }
  prefab.clips[static_cast<size_t>(clip)].name = std::move(name);
  return Result::Ok();
}

core::Result<int, CookError> MergeClips(prefab::Prefab& prefab,
                                        std::span<const int> clips,
                                        std::string name) {
  using Result = core::Result<int, CookError>;
  if (clips.empty() || name.empty()) {
    return Result::Error(CookError::kInvalidArgument);
  }
  std::vector<int> sorted(clips.begin(), clips.end());
  std::ranges::sort(sorted);
  if (std::ranges::adjacent_find(sorted) != sorted.end() ||
      !ValidIndex(prefab, sorted.front()) ||
      !ValidIndex(prefab, sorted.back())) {
    return Result::Error(CookError::kInvalidArgument);
  }

  std::vector<Clip> sources;
  sources.reserve(clips.size());
  for (const int index : clips) {
    sources.push_back(prefab.clips[static_cast<size_t>(index)]);
  }
  Clip merged = animation::MergeClips(sources, std::move(name));

  for (auto it = sorted.rbegin(); it != sorted.rend(); ++it) {
    prefab.clips.erase(prefab.clips.begin() + *it);
  }
  const int position = sorted.front();
  prefab.clips.insert(prefab.clips.begin() + position, std::move(merged));
  return Result::Ok(position);
}

core::Result<void, CookError> TrimClip(prefab::Prefab& prefab, int clip,
                                       float start, float end) {
  using Result = core::Result<void, CookError>;
  if (!ValidIndex(prefab, clip) || !std::isfinite(start) ||
      !std::isfinite(end) || start < 0.0f || !(start < end)) {
    return Result::Error(CookError::kInvalidArgument);
  }
  Clip& target = prefab.clips[static_cast<size_t>(clip)];
  Clip trimmed{
      .name = target.name, .duration = end - start, .loop = target.loop};
  trimmed.channels.reserve(target.channels.size());
  for (const Channel& channel : target.channels) {
    trimmed.channels.push_back(TrimChannel(channel, start, end));
  }
  for (const animation::ClipEvent& event : target.events) {
    if (event.time >= start - kTimeEpsilon &&
        event.time <= end + kTimeEpsilon) {
      trimmed.events.push_back(
          {.name = event.name,
           .time = std::clamp(event.time - start, 0.0f, end - start)});
    }
  }
  target = std::move(trimmed);
  return Result::Ok();
}

core::Result<void, CookError> SetClipLoop(prefab::Prefab& prefab, int clip,
                                          bool loop) {
  using Result = core::Result<void, CookError>;
  if (!ValidIndex(prefab, clip)) {
    return Result::Error(CookError::kInvalidArgument);
  }
  prefab.clips[static_cast<size_t>(clip)].loop = loop;
  return Result::Ok();
}

core::Result<int, CookError> AddClipEvent(prefab::Prefab& prefab, int clip,
                                          std::string name, float time) {
  using Result = core::Result<int, CookError>;
  if (!ValidIndex(prefab, clip) || name.empty() || !std::isfinite(time)) {
    return Result::Error(CookError::kInvalidArgument);
  }
  Clip& target = prefab.clips[static_cast<size_t>(clip)];
  if (time < 0.0f || time > target.duration) {
    return Result::Error(CookError::kInvalidArgument);
  }
  animation::AddEvent(target, {.name = std::move(name), .time = time});
  // AddEvent inserts after every event at the same or an earlier time.
  const auto position = std::ranges::upper_bound(target.events, time, {},
                                                 &animation::ClipEvent::time);
  return Result::Ok(
      static_cast<int>(std::distance(target.events.begin(), position)) - 1);
}

core::Result<void, CookError> RemoveClipEvent(prefab::Prefab& prefab, int clip,
                                              int event) {
  using Result = core::Result<void, CookError>;
  if (!ValidIndex(prefab, clip)) {
    return Result::Error(CookError::kInvalidArgument);
  }
  Clip& target = prefab.clips[static_cast<size_t>(clip)];
  if (event < 0 || static_cast<size_t>(event) >= target.events.size()) {
    return Result::Error(CookError::kInvalidArgument);
  }
  target.events.erase(target.events.begin() + event);
  return Result::Ok();
}

}  // namespace blue::cook
