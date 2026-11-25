#include "player/movement.h"

#include <algorithm>

#include <glm/geometric.hpp>

#include "camera/look.h"

namespace blue::player {
namespace {

[[nodiscard]] float Axis(bool positive, bool negative) {
  return (positive ? 1.0f : 0.0f) - (negative ? 1.0f : 0.0f);
}

}  // namespace

bool IsMoving(const MoveInput& input) {
  return Axis(input.right, input.left) != 0.0f ||
         Axis(input.forward, input.back) != 0.0f;
}

MoveIntent ComputeMove(const MoveInput& input, const PlayerTuning& tuning) {
  const float move_x = Axis(input.right, input.left);
  const float move_z = Axis(input.forward, input.back);
  const bool moving = move_x != 0.0f || move_z != 0.0f;

  MoveIntent intent;
  intent.aiming = input.aim;
  intent.sprinting = input.sprint && moving && !input.crouching && !input.aim;

  float speed = intent.sprinting ? tuning.sprint_speed : tuning.walk_speed;
  if (input.aim) {
    speed = std::min(speed, tuning.aim_speed);
  }
  if (input.crouching) {
    speed = std::min(speed, tuning.crouch_speed);
  }
  glm::vec3 direction = camera::FlatForwardFrom(input.yaw) * move_z +
                        camera::FlatRightFrom(input.yaw) * move_x;
  const float length = glm::length(direction);
  if (length > 1.0f) {
    direction /= length;
  }
  intent.velocity = direction * speed;
  intent.jump = input.jump && !input.crouching;
  return intent;
}

}  // namespace blue::player
