#include "game/inspect_screen_rules.h"

#include <glm/vec2.hpp>

#include "input/action.h"
#include "input/input_frame.h"

namespace blue::game {
namespace {

// The actions that do not reach the player or the weapon.
constexpr input::Action kBlocked[] = {
    input::Action::kMoveForward, input::Action::kMoveBack,
    input::Action::kMoveLeft,    input::Action::kMoveRight,
    input::Action::kFlyUp,       input::Action::kFlyDown,
    input::Action::kJump,        input::Action::kSprint,
    input::Action::kFire,        input::Action::kAim,
    input::Action::kReload,      input::Action::kToggleFireMode,
    input::Action::kEquipPistol, input::Action::kEquipRifle,
    input::Action::kInteract,    input::Action::kDropWeapon,
    input::Action::kZeroUp,      input::Action::kZeroDown,
    input::Action::kPush,
};

}  // namespace

bool NextInspectOpen(const InspectToggleInput& input) {
  if (input.open) {
    return !input.toggle_pressed && !input.escape_pressed &&
           input.player_view && input.weapon_in_hand;
  }
  return input.toggle_pressed && input.player_view && input.weapon_in_hand &&
         !input.switching;
}

input::InputFrame GateInspectInput(const input::InputFrame& frame) {
  input::InputFrame gated;  // No move, look, pan, wheel or mouse motion.
  gated.actions = frame.actions;
  for (const input::Action action : kBlocked) {
    gated.actions.Forget(action);
  }
  return gated;
}

glm::vec2 InspectTurn(const input::InputFrame& frame) {
  if (!frame.look) {
    return glm::vec2(0.0f);
  }
  // SDL's +y is down: a drag up pitches up.
  return glm::vec2(frame.mouse_delta.x, -frame.mouse_delta.y) *
         kInspectRadiansPerCount;
}

}  // namespace blue::game
