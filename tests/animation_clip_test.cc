#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <numbers>
#include <vector>

#include <glm/ext/quaternion_geometric.hpp>
#include <glm/ext/quaternion_trigonometric.hpp>

#include "animation/clip.h"
#include "animation/transform.h"

namespace blue::animation {
namespace {

constexpr float kEpsilon = 1.0e-4f;

bool NearlyEqual(const glm::vec3& a, const glm::vec3& b) {
  return glm::all(glm::lessThanEqual(glm::abs(a - b), glm::vec3(kEpsilon)));
}

// True if `a` and `b` are the same rotation (q and -q are equal).
bool SameRotation(const glm::quat& a, const glm::quat& b) {
  return std::abs(std::abs(glm::dot(a, b)) - 1.0f) <= kEpsilon;
}

// Returns the rotation about +Y by `degrees`.
glm::quat AboutY(float degrees) {
  return glm::angleAxis(degrees * std::numbers::pi_v<float> / 180.0f,
                        glm::vec3(0.0f, 1.0f, 0.0f));
}

// Appends `q` to `values` in glTF order (x, y, z, w).
void PushQuat(const glm::quat& q, std::vector<float>& values) {
  values.insert(values.end(), {q.x, q.y, q.z, q.w});
}

// Translation x keyed 0 at t=1 and 10 at t=2 (y and z stay 0) on node 0.
Channel TwoKeyTranslation(Interpolation interpolation) {
  return {.node = 0,
          .path = TargetPath::kTranslation,
          .interpolation = interpolation,
          .times = {1.0f, 2.0f},
          .values = {0.0f, 0.0f, 0.0f, 10.0f, 0.0f, 0.0f}};
}

// Returns pose[0] after sampling a clip of the single `channel` at `time`.
Transform SampleOne(const Channel& channel, float time,
                    const Transform& start = {}) {
  const Clip clip{.name = "test", .duration = 2.0f, .channels = {channel}};
  std::array<Transform, 1> pose{start};
  SampleClip(clip, time, pose);
  return pose[0];
}

}  // namespace

TEST_CASE("Channel validity checks layout and key order") {
  CHECK(IsValid(TwoKeyTranslation(Interpolation::kLinear)));
  Channel channel = TwoKeyTranslation(Interpolation::kLinear);
  channel.values.pop_back();
  CHECK_FALSE(IsValid(channel));
  channel = TwoKeyTranslation(Interpolation::kCubicSpline);
  CHECK_FALSE(IsValid(channel));  // needs three elements per key
  channel = TwoKeyTranslation(Interpolation::kLinear);
  channel.times = {2.0f, 2.0f};
  CHECK_FALSE(IsValid(channel));
  CHECK_FALSE(IsValid(Channel{}));
  CHECK(ComponentCount(TargetPath::kRotation) == 4);
  CHECK(ComponentCount(TargetPath::kScale) == 3);
}

TEST_CASE("Linear translation interpolates and clamps") {
  const Channel channel = TwoKeyTranslation(Interpolation::kLinear);
  CHECK(NearlyEqual(SampleOne(channel, 1.25f).translation, {2.5f, 0, 0}));
  CHECK(NearlyEqual(SampleOne(channel, 1.0f).translation, {0, 0, 0}));
  CHECK(NearlyEqual(SampleOne(channel, 2.0f).translation, {10, 0, 0}));
  // Before the first and after the last key.
  CHECK(NearlyEqual(SampleOne(channel, -5.0f).translation, {0, 0, 0}));
  CHECK(NearlyEqual(SampleOne(channel, 0.5f).translation, {0, 0, 0}));
  CHECK(NearlyEqual(SampleOne(channel, 100.0f).translation, {10, 0, 0}));
}

TEST_CASE("Step holds the earlier key") {
  const Channel channel = TwoKeyTranslation(Interpolation::kStep);
  CHECK(NearlyEqual(SampleOne(channel, 1.0f).translation, {0, 0, 0}));
  CHECK(NearlyEqual(SampleOne(channel, 1.99f).translation, {0, 0, 0}));
  CHECK(NearlyEqual(SampleOne(channel, 2.0f).translation, {10, 0, 0}));
}

TEST_CASE("Linear sampling finds the right interval among many keys") {
  Channel channel{.node = 0,
                  .path = TargetPath::kScale,
                  .interpolation = Interpolation::kLinear};
  for (int i = 0; i < 45; ++i) {
    const auto key = static_cast<float>(i);
    channel.times.push_back(key / 30.0f);
    channel.values.insert(channel.values.end(), {key, 1.0f, 1.0f});
  }
  REQUIRE(IsValid(channel));
  // Halfway between keys 20 and 21.
  CHECK(NearlyEqual(SampleOne(channel, 20.5f / 30.0f).scale, {20.5f, 1, 1}));
  CHECK(NearlyEqual(SampleOne(channel, 44.0f / 30.0f).scale, {44, 1, 1}));
}

TEST_CASE("Cubic spline follows the glTF Hermite form") {
  // Keys at t=1 (value 0, out-tangent 3/s) and t=3 (value 10, in-tangent
  // 0/s). With interval 2: p(s) = h00*0 + h10*2*3 + h01*10 + h11*2*0.
  Channel channel{
      .node = 0,
      .path = TargetPath::kTranslation,
      .interpolation = Interpolation::kCubicSpline,
      .times = {1.0f, 3.0f},
      .values = {// key 0: in-tangent, value, out-tangent
                 9.0f, 9.0f, 9.0f, 0.0f, 0.0f, 0.0f, 3.0f, 0.0f, 0.0f,
                 // key 1
                 0.0f, 0.0f, 0.0f, 10.0f, 0.0f, 0.0f, 9.0f, 9.0f, 9.0f}};
  REQUIRE(IsValid(channel));
  // s = 0.5: h10 = 0.125, h01 = 0.5 -> 0.125 * 6 + 5 = 5.75.
  CHECK(NearlyEqual(SampleOne(channel, 2.0f).translation, {5.75f, 0, 0}));
  // Exactly on a key gives the value, not a tangent.
  CHECK(NearlyEqual(SampleOne(channel, 1.0f).translation, {0, 0, 0}));
  CHECK(NearlyEqual(SampleOne(channel, 3.0f).translation, {10, 0, 0}));
  CHECK(NearlyEqual(SampleOne(channel, 0.0f).translation, {0, 0, 0}));
  CHECK(NearlyEqual(SampleOne(channel, 4.0f).translation, {10, 0, 0}));
}

TEST_CASE("Cubic spline rotation is normalised") {
  Channel channel{.node = 0,
                  .path = TargetPath::kRotation,
                  .interpolation = Interpolation::kCubicSpline,
                  .times = {0.0f, 1.0f}};
  const glm::quat zero{0.0f, 0.0f, 0.0f, 0.0f};
  for (const glm::quat& key : {AboutY(0.0f), AboutY(90.0f)}) {
    PushQuat(zero, channel.values);
    PushQuat(key, channel.values);
    PushQuat(zero, channel.values);
  }
  REQUIRE(IsValid(channel));
  const glm::quat sampled = SampleOne(channel, 0.5f).rotation;
  CHECK(std::abs(glm::length(sampled) - 1.0f) <= kEpsilon);
  CHECK(SameRotation(sampled, AboutY(45.0f)));
}

TEST_CASE("Linear rotation slerps along the shortest path") {
  Channel channel{.node = 0,
                  .path = TargetPath::kRotation,
                  .interpolation = Interpolation::kLinear,
                  .times = {0.0f, 1.0f}};
  PushQuat(AboutY(10.0f), channel.values);
  // 350 degrees stored with the opposite sign: still only 20 degrees away.
  PushQuat(-AboutY(-10.0f), channel.values);
  REQUIRE(IsValid(channel));

  const glm::quat mid = SampleOne(channel, 0.5f).rotation;
  CHECK(SameRotation(mid, AboutY(0.0f)));
  CHECK(SameRotation(SampleOne(channel, 0.25f).rotation, AboutY(5.0f)));
  CHECK(std::abs(glm::length(mid) - 1.0f) <= kEpsilon);
}

TEST_CASE("A single-key channel is constant") {
  const Channel channel{.node = 0,
                        .path = TargetPath::kTranslation,
                        .interpolation = Interpolation::kLinear,
                        .times = {0.5f},
                        .values = {1.0f, 2.0f, 3.0f}};
  for (const float time : {-1.0f, 0.5f, 7.0f}) {
    CHECK(NearlyEqual(SampleOne(channel, time).translation, {1, 2, 3}));
  }
}

TEST_CASE("SampleClip touches only targeted components and nodes") {
  const Transform start{.translation = {4.0f, 4.0f, 4.0f},
                        .rotation = AboutY(30.0f),
                        .scale = {2.0f, 2.0f, 2.0f}};
  const Clip clip{.name = "move",
                  .duration = 2.0f,
                  .channels = {TwoKeyTranslation(Interpolation::kLinear)}};
  std::array<Transform, 2> pose{start, start};
  SampleClip(clip, 1.5f, pose);

  CHECK(NearlyEqual(pose[0].translation, {5.0f, 0.0f, 0.0f}));
  CHECK(SameRotation(pose[0].rotation, start.rotation));
  CHECK(NearlyEqual(pose[0].scale, start.scale));
  CHECK(NearlyEqual(pose[1].translation, start.translation));

  // A channel whose node does not fit in the pose is skipped.
  Clip far = clip;
  far.channels[0].node = 5;
  SampleClip(far, 1.5f, pose);
  CHECK(NearlyEqual(pose[1].translation, start.translation));
}

TEST_CASE("MergeClips concatenates channels and keeps the longest duration") {
  Channel second = TwoKeyTranslation(Interpolation::kLinear);
  second.node = 1;
  const std::array<Clip, 2> clips{
      Clip{.name = "a",
           .duration = 2.0f,
           .channels = {TwoKeyTranslation(Interpolation::kLinear)}},
      Clip{.name = "b", .duration = 1.5f, .channels = {second}}};

  const Clip merged = MergeClips(clips, "fire");

  CHECK(merged.name == "fire");
  CHECK(merged.duration == 2.0f);
  REQUIRE(merged.channels.size() == 2);
  CHECK(merged.channels[0].node == 0);
  CHECK(merged.channels[1].node == 1);
  CHECK(MergeClips({}, "empty").channels.empty());
}

TEST_CASE("MergeClips merges events in time order and loops only if all do") {
  Clip first{.name = "a", .duration = 1.0f, .loop = true};
  AddEvent(first, {.name = "late", .time = 0.8f});
  Clip second{.name = "b", .duration = 1.0f, .loop = true};
  AddEvent(second, {.name = "early", .time = 0.1f});
  AddEvent(second, {.name = "tie", .time = 0.8f});

  std::array<Clip, 2> clips{first, second};
  Clip merged = MergeClips(clips, "m");
  REQUIRE(merged.events.size() == 3);
  CHECK(merged.events[0].name == "early");
  CHECK(merged.events[1].name == "late");  // ties keep clip order
  CHECK(merged.events[2].name == "tie");
  CHECK(merged.loop);
  CHECK(IsValid(merged));

  clips[1].loop = false;
  CHECK_FALSE(MergeClips(clips, "m").loop);
  CHECK_FALSE(MergeClips({}, "empty").loop);
}

TEST_CASE("AddEvent keeps events sorted and FindEvent looks them up by name") {
  Clip clip{.name = "fire", .duration = 0.5f};
  AddEvent(clip, {.name = "slide_lock", .time = 0.3f});
  AddEvent(clip, {.name = "shot", .time = 0.0f});
  AddEvent(clip, {.name = "eject", .time = 0.3f});

  REQUIRE(clip.events.size() == 3);
  CHECK(clip.events[0].name == "shot");
  CHECK(clip.events[1].name == "slide_lock");
  CHECK(clip.events[2].name == "eject");  // same time: after the earlier one

  const ClipEvent* lock = FindEvent(clip, "slide_lock");
  REQUIRE(lock != nullptr);
  CHECK(lock->time == 0.3f);
  CHECK(FindEvent(clip, "missing") == nullptr);
}

TEST_CASE("IsValid rejects bad durations, channels and unsorted events") {
  Clip clip{.name = "c",
            .duration = 1.0f,
            .channels = {TwoKeyTranslation(Interpolation::kLinear)}};
  CHECK(IsValid(clip));

  Clip negative = clip;
  negative.duration = -1.0f;
  CHECK_FALSE(IsValid(negative));

  Clip bad_channel = clip;
  bad_channel.channels[0].values.pop_back();
  CHECK_FALSE(IsValid(bad_channel));

  Clip unsorted = clip;
  unsorted.events = {{.name = "b", .time = 0.5f}, {.name = "a", .time = 0.1f}};
  CHECK_FALSE(IsValid(unsorted));

  Clip non_finite = clip;
  non_finite.events = {{.name = "a", .time = std::nanf("")}};
  CHECK_FALSE(IsValid(non_finite));
}

}  // namespace blue::animation
