#include "player/movement.h"

#include <doctest/doctest.h>

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/vec3.hpp>

#include "camera/look.h"
#include "player/player_controller.h"

namespace blue::player {
namespace {

float Speed(const MoveIntent& intent) { return glm::length(intent.velocity); }

TEST_CASE("ComputeMove picks the speed of the gait") {
  const PlayerTuning tuning;
  MoveInput input{.forward = true};

  SUBCASE("walk") {
    const MoveIntent intent = ComputeMove(input, tuning);
    CHECK(Speed(intent) == doctest::Approx(tuning.walk_speed));
    CHECK_FALSE(intent.sprinting);
    CHECK(intent.velocity.y == 0.0f);
  }
  SUBCASE("sprint") {
    input.sprint = true;
    const MoveIntent intent = ComputeMove(input, tuning);
    CHECK(Speed(intent) == doctest::Approx(tuning.sprint_speed));
    CHECK(intent.sprinting);
  }
  SUBCASE("aiming caps the speed and blocks sprinting") {
    input.sprint = true;
    input.aim = true;
    const MoveIntent intent = ComputeMove(input, tuning);
    CHECK(Speed(intent) == doctest::Approx(tuning.aim_speed));
    CHECK_FALSE(intent.sprinting);
    CHECK(intent.aiming);
  }
  SUBCASE("crouching caps the speed and blocks sprinting") {
    input.sprint = true;
    input.crouching = true;
    const MoveIntent intent = ComputeMove(input, tuning);
    CHECK(Speed(intent) == doctest::Approx(tuning.crouch_speed));
    CHECK_FALSE(intent.sprinting);
  }
  SUBCASE("aiming while crouched takes the slower cap") {
    input.aim = true;
    input.crouching = true;
    CHECK(Speed(ComputeMove(input, tuning)) ==
          doctest::Approx(tuning.crouch_speed));
  }
}

TEST_CASE("ComputeMove does not sprint standing still") {
  const MoveIntent intent =
      ComputeMove(MoveInput{.sprint = true}, PlayerTuning{});
  CHECK_FALSE(intent.sprinting);
  CHECK(intent.velocity == glm::vec3(0.0f));
}

TEST_CASE("diagonal movement is not faster") {
  const PlayerTuning tuning;
  const MoveIntent intent =
      ComputeMove(MoveInput{.forward = true, .right = true}, tuning);
  CHECK(Speed(intent) == doctest::Approx(tuning.walk_speed));
  CHECK(intent.velocity.x != 0.0f);
  CHECK(intent.velocity.z != 0.0f);
}

TEST_CASE("opposite keys cancel out") {
  const MoveInput input{.forward = true, .back = true, .sprint = true};
  CHECK_FALSE(IsMoving(input));
  const MoveIntent intent = ComputeMove(input, PlayerTuning{});
  CHECK(intent.velocity == glm::vec3(0.0f));
  CHECK_FALSE(intent.sprinting);
  CHECK(IsMoving(MoveInput{.left = true}));
}

TEST_CASE("the direction follows the yaw") {
  const PlayerTuning tuning;
  for (const float yaw : {0.0f, glm::half_pi<float>(), -2.0f}) {
    const MoveIntent forward =
        ComputeMove(MoveInput{.forward = true, .yaw = yaw}, tuning);
    const glm::vec3 expected = camera::FlatForwardFrom(yaw) * tuning.walk_speed;
    CHECK(glm::length(forward.velocity - expected) < 1e-5f);

    const MoveIntent right =
        ComputeMove(MoveInput{.right = true, .yaw = yaw}, tuning);
    const glm::vec3 expected_right =
        camera::FlatRightFrom(yaw) * tuning.walk_speed;
    CHECK(glm::length(right.velocity - expected_right) < 1e-5f);
  }
}

TEST_CASE("no jump while crouched") {
  const PlayerTuning tuning;
  CHECK(ComputeMove(MoveInput{.jump = true}, tuning).jump);
  CHECK_FALSE(
      ComputeMove(MoveInput{.jump = true, .crouching = true}, tuning).jump);
  CHECK_FALSE(ComputeMove(MoveInput{}, tuning).jump);
}

}  // namespace
}  // namespace blue::player
