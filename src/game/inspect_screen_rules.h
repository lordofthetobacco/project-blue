#ifndef BLUE_GAME_INSPECT_SCREEN_RULES_H_
#define BLUE_GAME_INSPECT_SCREEN_RULES_H_

#include <glm/vec2.hpp>

#include "input/input_frame.h"

// When the weapon inspect screen opens and closes, and what of the player's
// input gets through while it is open.

namespace blue::game {

// The weapon turns by this many radians per mouse count dragged.
inline constexpr float kInspectRadiansPerCount = 0.008f;

// What decides whether the inspect screen is open this frame.
struct InspectToggleInput {
  bool open = false;            // open last frame
  bool toggle_pressed = false;  // kInspectWeapon
  bool escape_pressed = false;  // kEscape
  bool player_view = false;     // the player view, with a character
  bool weapon_in_hand = false;
  bool switching = false;  // a weapon switch is running
};

// Returns whether the screen is open: an open screen closes on the toggle,
// on Escape, out of the player view and when the weapon leaves the hands;
// a closed one opens on the toggle in the player view with a weapon in
// hand and no switch running.
[[nodiscard]] bool NextInspectOpen(const InspectToggleInput& input);

// Returns `frame` as the game sees it while the screen is open: no
// movement, look, wheel, firing, aiming, reloading, fire-mode, weapon
// switch, interaction, drop, zero or push input. The other actions (the
// screen's own, Escape, the debug and display keys, the camera cycle and
// a held crouch) get through.
[[nodiscard]] input::InputFrame GateInspectInput(
    const input::InputFrame& frame);

// Returns how far a drag in `frame` turns the inspected weapon, as (yaw,
// pitch) radians: + yaw for a drag to the right, + pitch for a drag up.
// Zero unless the frame drags (InputFrame::look).
[[nodiscard]] glm::vec2 InspectTurn(const input::InputFrame& frame);

}  // namespace blue::game

#endif  // BLUE_GAME_INSPECT_SCREEN_RULES_H_
