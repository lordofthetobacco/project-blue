#include "weapon/weapon_animator.h"

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "animation/clip.h"
#include "animation/hierarchy.h"
#include "animation/transform.h"
#include "asset/animated_model.h"
#include "core/path.h"
#include "core/result.h"

namespace blue::weapon {
namespace {

constexpr float kFrame = 1.0f / 60.0f;

// A root with a slide (rest x 0) and a trigger. The clip: the trigger moves
// down from 0.1 s to 0.2 s and back by 0.5 s; the slide runs back from
// 0.2 s to +1 at 0.3 s and home by 0.4 s; the clip lasts 1 s.
struct TestRig {
  animation::Hierarchy hierarchy;
  animation::Clip clip;
};

TestRig MakeRig() {
  TestRig rig;
  rig.hierarchy.nodes = {
      {.name = "Root", .parent = -1, .rest = {.translation = {0, 5, 0}}},
      {.name = "Slide", .parent = 0, .rest = {.translation = {0, 1, 0}}},
      {.name = "Trigger", .parent = 0, .rest = {}},
  };
  rig.clip.name = "fire";
  rig.clip.duration = 1.0f;
  rig.clip.channels.push_back({
      .node = 1,
      .path = animation::TargetPath::kTranslation,
      .interpolation = animation::Interpolation::kLinear,
      .times = {0.0f, 0.2f, 0.3f, 0.4f, 1.0f},
      .values = {0, 1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 0, 0, 1, 0},
  });
  rig.clip.channels.push_back({
      .node = 2,
      .path = animation::TargetPath::kTranslation,
      .interpolation = animation::Interpolation::kLinear,
      .times = {0.0f, 0.1f, 0.2f, 0.5f, 1.0f},
      .values = {0, 0, 0, 0, 0, 0, 0, -1, 0, 0, 0, 0, 0, 0, 0},
  });
  return rig;
}

WeaponAnimatorTuning RigTuning() {
  return {.fire_time = 0.2f, .slide_node = "Slide", .lock_time = 0.3f};
}

float SlideX(const WeaponAnimator& animator) {
  return animator.pose()[1].translation.x;
}

TEST_CASE("MeasureFiringKeys finds where the slide starts and is rearmost") {
  const TestRig rig = MakeRig();
  const FiringKeyTimes keys = MeasureFiringKeys(rig.clip, 1);
  CHECK(keys.motion_start == doctest::Approx(0.2f));
  CHECK(keys.extreme == doctest::Approx(0.3f));
  const FiringKeyTimes none = MeasureFiringKeys(rig.clip, 0);
  CHECK(none.motion_start == -1.0f);
  CHECK(none.extreme == -1.0f);
}

TEST_CASE("the animator rests in the rest pose until a shot") {
  const TestRig rig = MakeRig();
  WeaponAnimator animator(&rig.hierarchy, &rig.clip, RigTuning());
  CHECK(animator.slide_node() == 1);
  REQUIRE(animator.pose().size() == 3);
  animator.Update(0, false, kFrame);
  CHECK_FALSE(animator.playing());
  CHECK(animator.pose()[0].translation == glm::vec3(0, 5, 0));
  CHECK(animator.pose()[1].translation == glm::vec3(0, 1, 0));
}

TEST_CASE("a shot restarts the clip at the fire time and it plays out") {
  const TestRig rig = MakeRig();
  WeaponAnimator animator(&rig.hierarchy, &rig.clip, RigTuning());
  animator.Update(1, false, kFrame);
  CHECK(animator.playing());
  CHECK(animator.time() == doctest::Approx(0.2f));
  // The trigger is held back at the break; the slide has not moved yet.
  CHECK(animator.pose()[2].translation.y == doctest::Approx(-1.0f));
  CHECK(SlideX(animator) == doctest::Approx(0.0f));
  animator.Update(0, false, 0.1f);
  CHECK(SlideX(animator) == doctest::Approx(1.0f));
  animator.Update(0, false, 0.05f);
  CHECK(SlideX(animator) == doctest::Approx(0.5f));
  // Several shots in one frame restart once.
  animator.Update(3, false, kFrame);
  CHECK(animator.time() == doctest::Approx(0.2f));
  // To the end, then the rest pose.
  animator.Update(0, false, 2.0f);
  CHECK_FALSE(animator.playing());
  CHECK(animator.pose()[1].translation == glm::vec3(0, 1, 0));
  CHECK(animator.pose()[2].translation == glm::vec3(0, 0, 0));
}

TEST_CASE("a locked-back bolt holds the slide rearmost") {
  const TestRig rig = MakeRig();
  WeaponAnimator animator(&rig.hierarchy, &rig.clip, RigTuning());
  // The last shot: the slide still travels back first.
  animator.Update(1, true, kFrame);
  CHECK(SlideX(animator) == doctest::Approx(0.0f));
  animator.Update(0, true, 0.05f);
  CHECK(SlideX(animator) == doctest::Approx(0.5f));
  // Past the rearmost key it stays there while the rest plays on.
  animator.Update(0, true, 0.15f);
  CHECK(SlideX(animator) == doctest::Approx(1.0f));
  CHECK(animator.pose()[2].translation.y == doctest::Approx(-1.0f / 3.0f));
  animator.Update(0, true, 3.0f);
  CHECK_FALSE(animator.playing());
  CHECK(SlideX(animator) == doctest::Approx(1.0f));
  // Released by the reload: home.
  animator.Update(0, false, kFrame);
  CHECK(SlideX(animator) == doctest::Approx(0.0f));
  // Locked without a shot (e.g. drawn empty): rearmost at once.
  animator.Reset();
  CHECK(SlideX(animator) == doctest::Approx(0.0f));
  animator.Update(0, true, kFrame);
  CHECK(SlideX(animator) == doctest::Approx(1.0f));
}

TEST_CASE("ComputeWorld applies the root matrix to the node transforms") {
  const TestRig rig = MakeRig();
  WeaponAnimator animator(&rig.hierarchy, &rig.clip, RigTuning());
  animator.Update(1, false, kFrame);
  animator.Update(0, false, 0.1f);  // slide rearmost: local (1, 1, 0)
  const glm::mat4 root =
      glm::translate(glm::mat4(1.0f), glm::vec3(10.0f, 0.0f, 0.0f)) *
      glm::scale(glm::mat4(1.0f), glm::vec3(2.0f));
  std::array<glm::mat4, 4> world{};
  world[3] = glm::mat4(7.0f);
  animator.ComputeWorld(root, world);
  const glm::vec3 slide = glm::vec3(world[1] * glm::vec4(0, 0, 0, 1));
  CHECK(slide.x == doctest::Approx(12.0f));
  CHECK(slide.y == doctest::Approx(12.0f));
  CHECK(world[3] == glm::mat4(7.0f));  // beyond the nodes: untouched
}

TEST_CASE("the Glock's fire clip: shot at 0.333 s, slide rearmost at 0.4 s") {
  core::Result<asset::AnimatedModel, asset::ImportError> imported =
      asset::ImportAnimatedModel(
          core::JoinPath(BLUE_SAMPLES_DIR, "glock/glock17.glb"));
  REQUIRE(imported.ok());
  const asset::AnimatedModel model = std::move(imported).value();
  const animation::Clip fire = animation::MergeClips(model.clips, "fire");
  CHECK(fire.duration == doctest::Approx(1.5f).epsilon(0.01));
  const int slide = animation::FindNode(model.hierarchy, "Slide");
  REQUIRE(slide >= 0);
  const WeaponAnimatorTuning tuning;
  const FiringKeyTimes keys = MeasureFiringKeys(fire, slide);
  CHECK(keys.motion_start == doctest::Approx(tuning.fire_time).epsilon(0.01));
  CHECK(keys.extreme == doctest::Approx(tuning.lock_time).epsilon(0.01));

  WeaponAnimator animator(&model.hierarchy, &fire, tuning);
  CHECK(animator.slide_node() == slide);
  const float rest_x =
      model.hierarchy.nodes[static_cast<size_t>(slide)].rest.translation.x;
  animator.Update(1, false, 0.0f);
  animator.Update(0, false, tuning.lock_time - tuning.fire_time);
  // Rearmost: 0.5 units (42 mm, a Glock 17's slide travel is ~45 mm) back
  // toward +X, the shooter.
  const float travel =
      animator.pose()[static_cast<size_t>(slide)].translation.x - rest_x;
  CHECK(travel == doctest::Approx(0.5f).epsilon(0.02));
  animator.Update(0, false, 2.0f);
  CHECK(animator.pose()[static_cast<size_t>(slide)].translation.x ==
        doctest::Approx(rest_x));
}

}  // namespace
}  // namespace blue::weapon
