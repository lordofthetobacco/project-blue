#include <doctest/doctest.h>

#include <array>
#include <span>
#include <string>
#include <vector>

#include "animation/clip.h"
#include "animation/transform.h"
#include "cook/clip_tools.h"
#include "prefab/prefab.h"

namespace blue::cook {
namespace {

using animation::Channel;
using animation::Clip;
using animation::Interpolation;
using animation::TargetPath;

// A clip moving node `node` along x: 0 at t=0, 10 at t=1, 20 at t=2.
Clip MoveClip(std::string name, int node,
              Interpolation interpolation = Interpolation::kLinear) {
  Clip clip{.name = std::move(name), .duration = 2.0f};
  clip.channels.push_back({.node = node,
                           .path = TargetPath::kTranslation,
                           .interpolation = interpolation,
                           .times = {0.0f, 1.0f, 2.0f},
                           .values = {0, 0, 0, 10, 0, 0, 20, 0, 0}});
  return clip;
}

prefab::Prefab TwoNodePrefab() {
  prefab::Prefab prefab;
  prefab.hierarchy.nodes = {{.name = "a"}, {.name = "b", .parent = 0}};
  prefab.clips.push_back(MoveClip("walk", 0));
  prefab.clips.push_back(MoveClip("wave", 1));
  prefab.clips.push_back(MoveClip("idle", 1));
  return prefab;
}

float XAt(const Clip& clip, int node, float time) {
  std::array<animation::Transform, 2> pose{};
  animation::SampleClip(clip, time, pose);
  return pose[static_cast<size_t>(node)].translation.x;
}

TEST_CASE("RenameClip and SetClipLoop change one clip") {
  prefab::Prefab prefab = TwoNodePrefab();
  REQUIRE(RenameClip(prefab, 1, "fire").ok());
  CHECK(prefab.clips[1].name == "fire");
  CHECK(RenameClip(prefab, 1, "").error() == CookError::kInvalidArgument);
  CHECK(RenameClip(prefab, 3, "x").error() == CookError::kInvalidArgument);

  REQUIRE(SetClipLoop(prefab, 0, true).ok());
  CHECK(prefab.clips[0].loop);
  CHECK(!prefab.clips[1].loop);
  CHECK(SetClipLoop(prefab, -1, true).error() == CookError::kInvalidArgument);
}

TEST_CASE("MergeClips replaces the clips by one at the first position") {
  prefab::Prefab prefab = TwoNodePrefab();
  REQUIRE(AddClipEvent(prefab, 2, "blink", 0.5f).ok());
  const std::array<int, 2> merge = {2, 0};
  auto merged = MergeClips(prefab, merge, "all");
  REQUIRE(merged.ok());
  CHECK(merged.value() == 0);
  REQUIRE(prefab.clips.size() == 2);
  CHECK(prefab.clips[0].name == "all");
  CHECK(prefab.clips[0].channels.size() == 2);
  CHECK(prefab.clips[0].events.size() == 1);
  CHECK(prefab.clips[1].name == "wave");
  CHECK(animation::IsValid(prefab.clips[0]));
  CHECK(prefab::ValidatePrefab(prefab) == std::nullopt);
}

TEST_CASE("MergeClips rejects bad input and leaves the prefab unchanged") {
  prefab::Prefab prefab = TwoNodePrefab();
  const std::array<int, 2> repeated = {1, 1};
  const std::array<int, 2> out_of_range = {0, 3};
  CHECK(MergeClips(prefab, repeated, "x").error() ==
        CookError::kInvalidArgument);
  CHECK(MergeClips(prefab, out_of_range, "x").error() ==
        CookError::kInvalidArgument);
  CHECK(MergeClips(prefab, std::span<const int>(), "x").error() ==
        CookError::kInvalidArgument);
  const std::array<int, 1> one = {0};
  CHECK(MergeClips(prefab, one, "").error() == CookError::kInvalidArgument);
  CHECK(prefab.clips.size() == 3);
}

TEST_CASE("TrimClip cuts the range, re-bases keys and keeps the motion") {
  prefab::Prefab prefab = TwoNodePrefab();
  REQUIRE(AddClipEvent(prefab, 0, "early", 0.25f).ok());
  REQUIRE(AddClipEvent(prefab, 0, "middle", 1.0f).ok());
  REQUIRE(AddClipEvent(prefab, 0, "late", 1.75f).ok());
  REQUIRE(TrimClip(prefab, 0, 0.5f, 1.5f).ok());

  const Clip& clip = prefab.clips[0];
  CHECK(clip.duration == doctest::Approx(1.0f));
  REQUIRE(clip.channels.size() == 1);
  const Channel& channel = clip.channels[0];
  REQUIRE(animation::IsValid(channel));
  CHECK(channel.times == std::vector<float>{0.0f, 0.5f, 1.0f});
  for (const float t : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
    CAPTURE(t);
    CHECK(XAt(clip, 0, t) == doctest::Approx(10.0f * (t + 0.5f)));
  }
  REQUIRE(clip.events.size() == 1);
  CHECK(clip.events[0].name == "middle");
  CHECK(clip.events[0].time == doctest::Approx(0.5f));
  CHECK(animation::IsValid(clip));
}

TEST_CASE("TrimClip keeps keys lying on the boundaries") {
  prefab::Prefab prefab = TwoNodePrefab();
  REQUIRE(TrimClip(prefab, 0, 1.0f, 2.0f).ok());
  const Channel& channel = prefab.clips[0].channels[0];
  CHECK(channel.times == std::vector<float>{0.0f, 1.0f});
  CHECK(channel.values == std::vector<float>{10, 0, 0, 20, 0, 0});
}

TEST_CASE("TrimClip of step and cubic channels gives valid channels") {
  prefab::Prefab prefab;
  prefab.hierarchy.nodes = {{.name = "a"}};
  prefab.clips.push_back(MoveClip("step", 0, Interpolation::kStep));
  Clip cubic{.name = "cubic", .duration = 1.0f};
  cubic.channels.push_back(
      {.node = 0,
       .path = TargetPath::kTranslation,
       .interpolation = Interpolation::kCubicSpline,
       .times = {0.0f, 1.0f},
       .values = {0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 4, 0, 0, 1, 0, 0}});
  prefab.clips.push_back(cubic);

  REQUIRE(TrimClip(prefab, 0, 0.5f, 1.5f).ok());
  CHECK(XAt(prefab.clips[0], 0, 0.0f) == doctest::Approx(0.0f));
  CHECK(XAt(prefab.clips[0], 0, 0.6f) == doctest::Approx(10.0f));
  REQUIRE(TrimClip(prefab, 1, 0.25f, 0.75f).ok());
  const Channel& trimmed = prefab.clips[1].channels[0];
  CHECK(animation::IsValid(trimmed));
  CHECK(trimmed.values.size() == trimmed.times.size() * 9);
}

TEST_CASE("TrimClip rejects bad ranges") {
  prefab::Prefab prefab = TwoNodePrefab();
  CHECK(TrimClip(prefab, 0, 1.0f, 1.0f).error() == CookError::kInvalidArgument);
  CHECK(TrimClip(prefab, 0, -0.5f, 1.0f).error() ==
        CookError::kInvalidArgument);
  CHECK(TrimClip(prefab, 5, 0.0f, 1.0f).error() == CookError::kInvalidArgument);
  CHECK(prefab.clips[0].duration == 2.0f);
}

TEST_CASE("Clip events are added sorted and removed by index") {
  prefab::Prefab prefab = TwoNodePrefab();
  auto late = AddClipEvent(prefab, 0, "late", 1.5f);
  auto early = AddClipEvent(prefab, 0, "early", 0.5f);
  auto tie = AddClipEvent(prefab, 0, "late_too", 1.5f);
  REQUIRE(late.ok());
  REQUIRE(early.ok());
  REQUIRE(tie.ok());
  CHECK(late.value() == 0);
  CHECK(early.value() == 0);
  CHECK(tie.value() == 2);
  const auto& events = prefab.clips[0].events;
  REQUIRE(events.size() == 3);
  CHECK(events[0].name == "early");
  CHECK(events[1].name == "late");
  CHECK(events[2].name == "late_too");

  CHECK(AddClipEvent(prefab, 0, "after_end", 2.5f).error() ==
        CookError::kInvalidArgument);
  CHECK(AddClipEvent(prefab, 0, "", 1.0f).error() ==
        CookError::kInvalidArgument);

  REQUIRE(RemoveClipEvent(prefab, 0, 1).ok());
  REQUIRE(events.size() == 2);
  CHECK(events[1].name == "late_too");
  CHECK(RemoveClipEvent(prefab, 0, 2).error() == CookError::kInvalidArgument);
}

}  // namespace
}  // namespace blue::cook
