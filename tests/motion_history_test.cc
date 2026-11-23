#include "weapon/motion_history.h"

#include <doctest/doctest.h>

#include <limits>
#include <optional>
#include <vector>

#include <glm/ext/matrix_transform.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

namespace blue::weapon {
namespace {

// Returns `count` distinct transforms, shifted by `shift`.
std::vector<glm::mat4> Transforms(int count, float shift) {
  std::vector<glm::mat4> transforms;
  for (int i = 0; i < count; ++i) {
    transforms.push_back(glm::translate(
        glm::mat4(1.0f), glm::vec3(static_cast<float>(i), shift, 0.0f)));
  }
  return transforms;
}

MotionFrame Frame(const std::vector<glm::mat4>& transforms,
                  float vp_scale = 1.0f) {
  return {.slot = 1,
          .width = 1920,
          .height = 1080,
          .frame_seconds = 1.0f / 60.0f,
          .view_projection = glm::mat4(vp_scale),
          .transforms = transforms};
}

TEST_CASE("The first frame has no motion history") {
  MotionHistory history;
  const std::vector<glm::mat4> transforms = Transforms(3, 0.0f);
  CHECK_FALSE(history.Advance(Frame(transforms)).has_value());
}

TEST_CASE("Valid history returns exactly last frame's matrices") {
  MotionHistory history;
  const std::vector<glm::mat4> first = Transforms(3, 0.0f);
  const std::vector<glm::mat4> second = Transforms(3, 1.0f);
  const std::vector<glm::mat4> third = Transforms(3, 2.0f);
  CHECK_FALSE(history.Advance(Frame(first, 2.0f)).has_value());

  const std::optional<PreviousMotion> previous =
      history.Advance(Frame(second, 3.0f));
  REQUIRE(previous.has_value());
  CHECK(previous->view_projection == glm::mat4(2.0f));
  REQUIRE(previous->transforms.size() == first.size());
  for (size_t i = 0; i < first.size(); ++i) {
    CHECK(previous->transforms[i] == first[i]);
  }

  const std::optional<PreviousMotion> next =
      history.Advance(Frame(third, 4.0f));
  REQUIRE(next.has_value());
  CHECK(next->view_projection == glm::mat4(3.0f));
  for (size_t i = 0; i < second.size(); ++i) {
    CHECK(next->transforms[i] == second[i]);
  }
}

TEST_CASE("Invalidate drops the history for one frame") {
  MotionHistory history;
  const std::vector<glm::mat4> transforms = Transforms(2, 0.0f);
  static_cast<void>(history.Advance(Frame(transforms)));
  REQUIRE(history.Advance(Frame(transforms)).has_value());
  history.Invalidate();
  CHECK_FALSE(history.Advance(Frame(transforms)).has_value());
  CHECK(history.Advance(Frame(transforms)).has_value());
}

TEST_CASE("A different weapon slot invalidates the history") {
  MotionHistory history;
  const std::vector<glm::mat4> transforms = Transforms(2, 0.0f);
  static_cast<void>(history.Advance(Frame(transforms)));
  MotionFrame other = Frame(transforms);
  other.slot = 0;
  CHECK_FALSE(history.Advance(other).has_value());
  CHECK(history.Advance(other).has_value());
}

TEST_CASE("A resize invalidates the history") {
  MotionHistory history;
  const std::vector<glm::mat4> transforms = Transforms(2, 0.0f);
  static_cast<void>(history.Advance(Frame(transforms)));
  MotionFrame wider = Frame(transforms);
  wider.width = 1280;
  CHECK_FALSE(history.Advance(wider).has_value());
  MotionFrame taller = wider;
  taller.height = 720;
  CHECK_FALSE(history.Advance(taller).has_value());
  CHECK(history.Advance(taller).has_value());
}

TEST_CASE("A changed item count invalidates the history") {
  MotionHistory history;
  const std::vector<glm::mat4> three = Transforms(3, 0.0f);
  const std::vector<glm::mat4> four = Transforms(4, 0.0f);
  static_cast<void>(history.Advance(Frame(three)));
  CHECK_FALSE(history.Advance(Frame(four)).has_value());
  const std::optional<PreviousMotion> previous = history.Advance(Frame(four));
  REQUIRE(previous.has_value());
  CHECK(previous->transforms.size() == 4);
}

TEST_CASE("Zero, negative and non-finite frame durations give no history") {
  MotionHistory history;
  const std::vector<glm::mat4> first = Transforms(2, 0.0f);
  const std::vector<glm::mat4> second = Transforms(2, 1.0f);
  static_cast<void>(history.Advance(Frame(first)));
  for (const float seconds :
       {0.0f, -0.01f, std::numeric_limits<float>::quiet_NaN(),
        std::numeric_limits<float>::infinity()}) {
    MotionFrame frame = Frame(second);
    frame.frame_seconds = seconds;
    CHECK_FALSE(history.Advance(frame).has_value());
  }
  // Recorded anyway: the next normal frame blurs from the zero-length one.
  const std::optional<PreviousMotion> previous = history.Advance(Frame(first));
  REQUIRE(previous.has_value());
  CHECK(previous->transforms[0] == second[0]);
}

}  // namespace
}  // namespace blue::weapon
