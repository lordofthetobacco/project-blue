#ifndef BLUE_PLAYER_MOVEMENT_H_
#define BLUE_PLAYER_MOVEMENT_H_

#include <glm/vec3.hpp>

#include "player/player_controller.h"

namespace blue::player {

// One tick of player movement input with every toggle already resolved into
// plain held flags. Shared by the local controller, client prediction and the
// server so they all derive the same motion from the same input.
struct MoveInput {
  bool forward = false;
  bool back = false;
  bool left = false;
  bool right = false;
  bool sprint = false;  // Sprint requested (held, or latched by a toggle).
  bool aim = false;     // Aiming down the sights.
  bool jump = false;    // The jump key went down since the previous tick.
  // Stance the character is in for this move, i.e. after applying the crouch
  // request (standing up can be blocked by a ceiling).
  bool crouching = false;
  float yaw = 0.0f;  // radians, camera::LookAngles convention
};

// What the character should do this tick.
struct MoveIntent {
  glm::vec3 velocity{0.0f};  // Desired horizontal velocity, m/s (y is 0).
  bool jump = false;         // Request a jump of PlayerTuning::jump_speed.
  bool sprinting = false;
  bool aiming = false;
};

// Returns whether the movement keys of `input` add up to a direction
// (opposite keys cancel out).
[[nodiscard]] bool IsMoving(const MoveInput& input);

// Returns the movement for `input`: walk, sprint, aim or crouch speed (the
// slowest applicable one; sprinting needs movement and neither crouching nor
// aiming) along the input direction relative to `input.yaw`, normalized so
// diagonals are not faster. Jumps are refused while crouching.
[[nodiscard]] MoveIntent ComputeMove(const MoveInput& input,
                                     const PlayerTuning& tuning);

}  // namespace blue::player

#endif  // BLUE_PLAYER_MOVEMENT_H_
