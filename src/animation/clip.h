#ifndef BLUE_ANIMATION_CLIP_H_
#define BLUE_ANIMATION_CLIP_H_

#include <span>
#include <string>
#include <vector>

#include "animation/transform.h"

namespace blue::animation {

// How values between two keys are computed (glTF sampler interpolation).
enum class Interpolation {
  kStep,         // the earlier key's value until the next key
  kLinear,       // lerp; rotations slerp along the shortest path
  kCubicSpline,  // cubic Hermite with per-key in/out tangents
};

// Which component of a node's Transform a channel drives.
enum class TargetPath { kTranslation, kRotation, kScale };

// Returns the number of floats per element of `path`: 3 for translation and
// scale, 4 for rotation.
[[nodiscard]] int ComponentCount(TargetPath path);

// Keyframes for one component of one node, in glTF's sampler layout.
// `times` are strictly increasing seconds. `values` holds one element per key
// (kStep, kLinear) or three per key (kCubicSpline: in-tangent, value,
// out-tangent, in that order); an element is ComponentCount(path) floats,
// with rotations as x, y, z, w. Cubic tangents are per second, as in glTF.
struct Channel {
  int node = -1;  // index into the Hierarchy the clip was made for
  TargetPath path = TargetPath::kTranslation;
  Interpolation interpolation = Interpolation::kLinear;
  std::vector<float> times;
  std::vector<float> values;
};

// Returns true if `channel` has at least one key, its values match its key
// count and layout, and its times are strictly increasing.
[[nodiscard]] bool IsValid(const Channel& channel);

// A named set of channels played together. `duration` is in seconds,
// normally the last key time of any channel.
struct Clip {
  std::string name;
  float duration = 0.0f;
  std::vector<Channel> channels;
};

// Writes the value of every channel of `clip` at `time` (seconds) into the
// targeted component of pose[channel.node]. Components and nodes that no
// channel targets keep what `pose` held, so callers normally start from the
// rest pose (ResetToRest). Times before a channel's first key give its first
// value and times after its last key its last value. Rotations are
// normalised. Channels whose node is outside `pose` are skipped. Every channel
// must satisfy IsValid(). Does not allocate.
void SampleClip(const Clip& clip, float time, std::span<Transform> pose);

// Returns one clip named `name` holding the channels of all `clips`, with the
// longest duration among them. Used for files that store one action per node
// of what is really a single animation. Channels targeting the same node and
// path are kept in order, so the later one wins when sampled.
[[nodiscard]] Clip MergeClips(std::span<const Clip> clips, std::string name);

}  // namespace blue::animation

#endif  // BLUE_ANIMATION_CLIP_H_
