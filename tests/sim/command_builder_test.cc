#include "sim/command_builder.h"

#include <doctest/doctest.h>

#include <cstdint>

#include "camera/look.h"
#include "input/action.h"
#include "input/action_state.h"
#include "input/input_frame.h"
#include "input/input_settings.h"
#include "sim/user_command.h"

namespace blue::sim {
namespace {

using input::Action;

// The keyboard as the platform layer sees it: held state persists across
// frames, edges are cleared after every frame.
class Keys {
 public:
  void Press(Action action) { state_.SetHeld(action, true); }
  void Release(Action action) { state_.SetHeld(action, false); }
  void Tap(Action action) {
    Press(action);
    Release(action);
  }

  // Returns this frame's input and starts the next frame.
  input::InputFrame Frame() {
    input::InputFrame frame;
    frame.actions = state_;
    state_.ClearEdges();
    return frame;
  }

 private:
  input::ActionState state_;
};

class Fixture {
 public:
  // Ends the frame and feeds it to the builder.
  void EndFrame(camera::LookAngles look = {}) {
    builder.Accumulate(keys.Frame(), settings, look);
  }
  UserCommand Take() { return builder.Take(next_tick_++); }

  Keys keys;
  input::InputSettings settings;
  CommandBuilder builder;

 private:
  uint32_t next_tick_ = 0;
};

bool Has(uint32_t bits, uint32_t mask) { return (bits & mask) != 0; }

TEST_CASE("an empty builder yields an empty command") {
  CommandBuilder builder;
  const UserCommand command = builder.Take(42);
  CHECK(command == UserCommand{.tick = 42});
  CHECK(command.select_slot == -1);
}

TEST_CASE("a press in a frame without ticks reaches the next tick") {
  Fixture f;
  f.keys.Press(Action::kJump);
  f.EndFrame();  // No tick this frame.
  f.keys.Release(Action::kJump);
  f.EndFrame();
  CHECK(Has(f.Take().pressed, buttons::pressed::kJump));
  CHECK_FALSE(Has(f.Take().pressed, buttons::pressed::kJump));
}

TEST_CASE("a frame with several ticks gives its edges to the first only") {
  Fixture f;
  f.keys.Tap(Action::kReload);
  f.keys.Press(Action::kEquipRifle);
  f.EndFrame();
  const UserCommand first = f.Take();
  const UserCommand second = f.Take();
  CHECK(Has(first.pressed, buttons::pressed::kReload));
  CHECK(first.select_slot == 1);
  CHECK(second.pressed == 0u);
  CHECK(second.select_slot == -1);
}

TEST_CASE("a fire tap between two ticks holds fire for one tick") {
  Fixture f;
  SUBCASE("pressed and released in different frames") {
    f.keys.Press(Action::kFire);
    f.EndFrame();
    f.keys.Release(Action::kFire);
    f.EndFrame();
  }
  SUBCASE("pressed and released within one frame") {
    f.keys.Tap(Action::kFire);
    f.EndFrame();
  }
  CHECK(Has(f.Take().held, buttons::held::kFire));
  CHECK_FALSE(Has(f.Take().held, buttons::held::kFire));
}

TEST_CASE("held bits are those of the latest frame") {
  Fixture f;
  f.keys.Press(Action::kMoveForward);
  f.keys.Press(Action::kAim);
  f.EndFrame();
  UserCommand command = f.Take();
  CHECK(Has(command.held, buttons::held::kForward));
  CHECK(Has(command.held, buttons::held::kAim));

  // Still held: every tick sees it, without new edges.
  command = f.Take();
  CHECK(Has(command.held, buttons::held::kForward));

  f.keys.Release(Action::kMoveForward);
  f.EndFrame();
  command = f.Take();
  CHECK_FALSE(Has(command.held, buttons::held::kForward));
  CHECK(Has(command.held, buttons::held::kAim));
}

TEST_CASE("actions map to their bits") {
  Fixture f;
  const struct {
    Action action;
    uint32_t bit;
  } kHeld[] = {
      {Action::kMoveForward, buttons::held::kForward},
      {Action::kMoveBack, buttons::held::kBack},
      {Action::kMoveLeft, buttons::held::kLeft},
      {Action::kMoveRight, buttons::held::kRight},
      {Action::kCrouch, buttons::held::kCrouch},
      {Action::kSprint, buttons::held::kSprint},
      {Action::kFire, buttons::held::kFire},
      {Action::kAim, buttons::held::kAim},
  };
  for (const auto& entry : kHeld) {
    CommandBuilder builder;
    Keys keys;
    keys.Press(entry.action);
    builder.Accumulate(keys.Frame(), f.settings, {});
    (void)builder.Take(0);  // Drop the press edge.
    builder.Accumulate(keys.Frame(), f.settings, {});
    CHECK(builder.Take(1).held == entry.bit);
  }

  const struct {
    Action action;
    uint32_t bit;
  } kPressed[] = {
      {Action::kJump, buttons::pressed::kJump},
      {Action::kReload, buttons::pressed::kReload},
      {Action::kToggleFireMode, buttons::pressed::kFireMode},
      {Action::kDropWeapon, buttons::pressed::kDropWeapon},
      {Action::kInteract, buttons::pressed::kInteract},
      {Action::kZeroUp, buttons::pressed::kZeroUp},
      {Action::kZeroDown, buttons::pressed::kZeroDown},
  };
  for (const auto& entry : kPressed) {
    CommandBuilder builder;
    Keys keys;
    keys.Tap(entry.action);
    builder.Accumulate(keys.Frame(), f.settings, {});
    CHECK(builder.Take(0).pressed == entry.bit);
  }
}

TEST_CASE("the weapon selection is the latest one before the tick") {
  Fixture f;
  SUBCASE("a later frame overrides an earlier one") {
    f.keys.Tap(Action::kEquipPistol);
    f.EndFrame();
    f.keys.Tap(Action::kEquipRifle);
    f.EndFrame();
    CHECK(f.Take().select_slot == 1);
  }
  SUBCASE("a frame without selection keeps the earlier one") {
    f.keys.Tap(Action::kEquipPistol);
    f.EndFrame();
    f.EndFrame();
    CHECK(f.Take().select_slot == 0);
  }
  SUBCASE("the rifle wins within one frame") {
    f.keys.Tap(Action::kEquipPistol);
    f.keys.Tap(Action::kEquipRifle);
    f.EndFrame();
    CHECK(f.Take().select_slot == 1);
  }
  CHECK(f.Take().select_slot == -1);
}

TEST_CASE("the look and the tick come from the latest frame and Take") {
  Fixture f;
  f.EndFrame({.yaw = 0.5f, .pitch = 0.1f});
  f.EndFrame({.yaw = -1.0f, .pitch = 0.2f});
  const UserCommand command = f.builder.Take(1234);
  CHECK(command.tick == 1234u);
  CHECK(command.yaw == -1.0f);
  CHECK(command.pitch == 0.2f);
}

TEST_CASE("the crouch toggle becomes a held bit") {
  Fixture f;
  f.settings.crouch_toggle = true;
  f.keys.Tap(Action::kCrouch);
  f.EndFrame();
  CHECK(Has(f.Take().held, buttons::held::kCrouch));
  f.EndFrame();
  CHECK(Has(f.Take().held, buttons::held::kCrouch));
  f.keys.Tap(Action::kCrouch);
  f.EndFrame();
  CHECK_FALSE(Has(f.Take().held, buttons::held::kCrouch));
}

TEST_CASE("hold-to-crouch follows the key") {
  Fixture f;
  f.keys.Press(Action::kCrouch);
  f.EndFrame();
  CHECK(Has(f.Take().held, buttons::held::kCrouch));
  f.keys.Release(Action::kCrouch);
  f.EndFrame();
  CHECK_FALSE(Has(f.Take().held, buttons::held::kCrouch));
}

TEST_CASE("the sprint toggle becomes a held bit and ends like offline") {
  Fixture f;
  f.settings.sprint_toggle = true;
  f.keys.Press(Action::kMoveForward);
  f.keys.Tap(Action::kSprint);
  f.EndFrame();
  CHECK(Has(f.Take().held, buttons::held::kSprint));
  f.EndFrame();
  CHECK(Has(f.Take().held, buttons::held::kSprint));

  SUBCASE("stopping ends the sprint") {
    f.keys.Release(Action::kMoveForward);
    f.EndFrame();
    CHECK_FALSE(Has(f.Take().held, buttons::held::kSprint));
    f.keys.Press(Action::kMoveForward);
    f.EndFrame();
    CHECK_FALSE(Has(f.Take().held, buttons::held::kSprint));
  }
  SUBCASE("aiming ends the sprint") {
    f.keys.Press(Action::kAim);
    f.EndFrame();
    CHECK_FALSE(Has(f.Take().held, buttons::held::kSprint));
  }
  SUBCASE("a second press ends the sprint") {
    f.keys.Tap(Action::kSprint);
    f.EndFrame();
    CHECK_FALSE(Has(f.Take().held, buttons::held::kSprint));
  }
  SUBCASE("Reset forgets the latch") {
    f.builder.Reset();
    f.EndFrame();
    CHECK_FALSE(Has(f.Take().held, buttons::held::kSprint));
  }
}

}  // namespace
}  // namespace blue::sim
