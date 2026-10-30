#ifndef BLUE_INPUT_INPUT_FRAME_H_
#define BLUE_INPUT_INPUT_FRAME_H_

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include "input/action_state.h"

namespace blue::input {

// One frame of already-interpreted input. Produced by the platform layer
// (SDL), consumed by gameplay/camera.
struct InputFrame {
  // Summed relative motion in device counts, +x right, +y DOWN (SDL
  // convention).
  glm::vec2 mouse_delta{0.0f};
  // Summed vertical notches, + = away from the user.
  float wheel = 0.0f;
  // Held-key axes in [-1,1]: x right/left, y up/down, z forward/back.
  glm::vec3 move{0.0f};
  bool sprint = false;
  bool look = false;  // Capture mode: mouse captured. Drag mode: LMB held.
  bool pan = false;   // Drag mode: RMB or MMB held.
  // Held/pressed/released state of every bound action this frame. `move` and
  // `sprint` are derived from it for the camera's convenience.
  ActionState actions;
};

}  // namespace blue::input

#endif  // BLUE_INPUT_INPUT_FRAME_H_
