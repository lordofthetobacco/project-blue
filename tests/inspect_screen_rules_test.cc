#include "game/inspect_screen_rules.h"

#include <doctest/doctest.h>

#include <ostream>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include "input/action.h"
#include "input/input_frame.h"

namespace blue::game {
namespace {

using input::Action;

InspectToggleInput Ready() {
  return {.player_view = true, .weapon_in_hand = true};
}

TEST_CASE("the toggle opens the screen in the player view with a weapon") {
  InspectToggleInput input = Ready();
  CHECK_FALSE(NextInspectOpen(input));  // no press
  input.toggle_pressed = true;
  CHECK(NextInspectOpen(input));

  InspectToggleInput switching = input;
  switching.switching = true;
  CHECK_FALSE(NextInspectOpen(switching));
  InspectToggleInput empty_hands = input;
  empty_hands.weapon_in_hand = false;
  CHECK_FALSE(NextInspectOpen(empty_hands));
  InspectToggleInput debug_camera = input;
  debug_camera.player_view = false;
  CHECK_FALSE(NextInspectOpen(debug_camera));
  InspectToggleInput escape_only = Ready();
  escape_only.escape_pressed = true;
  CHECK_FALSE(NextInspectOpen(escape_only));
}

TEST_CASE("an open screen closes on the toggle, Escape or losing its weapon") {
  InspectToggleInput open = Ready();
  open.open = true;
  CHECK(NextInspectOpen(open));
  open.switching = true;  // a switch does not close it by itself
  CHECK(NextInspectOpen(open));

  InspectToggleInput toggle = open;
  toggle.toggle_pressed = true;
  CHECK_FALSE(NextInspectOpen(toggle));
  InspectToggleInput escape = open;
  escape.escape_pressed = true;
  CHECK_FALSE(NextInspectOpen(escape));
  InspectToggleInput dropped = open;
  dropped.weapon_in_hand = false;
  CHECK_FALSE(NextInspectOpen(dropped));
  InspectToggleInput left_view = open;
  left_view.player_view = false;
  CHECK_FALSE(NextInspectOpen(left_view));
}

TEST_CASE("the open screen keeps the player's game input out") {
  input::InputFrame frame;
  frame.mouse_delta = glm::vec2(5.0f, 3.0f);
  frame.wheel = 2.0f;
  frame.move = glm::vec3(1.0f, 0.0f, 1.0f);
  frame.sprint = true;
  frame.look = true;
  for (const Action action : input::AllActions()) {
    frame.actions.SetHeld(action, true);  // held and pressed
  }
  const input::InputFrame gated = GateInspectInput(frame);
  CHECK(gated.mouse_delta == glm::vec2(0.0f));
  CHECK(gated.wheel == 0.0f);
  CHECK(gated.move == glm::vec3(0.0f));
  CHECK_FALSE(gated.sprint);
  CHECK_FALSE(gated.look);
  CHECK_FALSE(gated.pan);
  for (const Action blocked :
       {Action::kMoveForward, Action::kMoveBack, Action::kMoveLeft,
        Action::kMoveRight, Action::kJump, Action::kSprint, Action::kFire,
        Action::kAim, Action::kReload, Action::kToggleFireMode,
        Action::kEquipPistol, Action::kEquipRifle, Action::kInteract,
        Action::kDropWeapon, Action::kZeroUp, Action::kZeroDown,
        Action::kPush}) {
    CAPTURE(input::ActionName(blocked));
    CHECK_FALSE(gated.actions.held(blocked));
    CHECK_FALSE(gated.actions.pressed(blocked));
  }
  for (const Action allowed :
       {Action::kInspectWeapon, Action::kEscape, Action::kToggleOverlay,
        Action::kLoadScene, Action::kCycleCamera, Action::kToggleVsync,
        Action::kCrouch}) {
    CAPTURE(input::ActionName(allowed));
    CHECK(gated.actions.held(allowed));
    CHECK(gated.actions.pressed(allowed));
  }
}

TEST_CASE("only a drag turns the inspected weapon") {
  input::InputFrame frame;
  frame.mouse_delta = glm::vec2(10.0f, -5.0f);  // right and up (SDL +y down)
  CHECK(InspectTurn(frame) == glm::vec2(0.0f));
  frame.look = true;
  const glm::vec2 turn = InspectTurn(frame);
  CHECK(turn.x == doctest::Approx(10.0f * kInspectRadiansPerCount));
  CHECK(turn.y == doctest::Approx(5.0f * kInspectRadiansPerCount));
}

}  // namespace
}  // namespace blue::game
