#include "sim/command_builder.h"

#include "input/action.h"
#include "input/action_state.h"
#include "player/movement.h"
#include "weapon/loadout.h"

namespace blue::sim {
namespace {

using input::Action;

struct ActionBit {
  Action action;
  uint32_t bit;
};

// Held inputs whose bit is the action's held flag. Crouch and sprint are
// handled separately because of their toggle settings.
constexpr ActionBit kHeldActions[] = {
    {Action::kMoveForward, buttons::held::kForward},
    {Action::kMoveBack, buttons::held::kBack},
    {Action::kMoveLeft, buttons::held::kLeft},
    {Action::kMoveRight, buttons::held::kRight},
    {Action::kFire, buttons::held::kFire},
    {Action::kAim, buttons::held::kAim},
};

constexpr ActionBit kPressedActions[] = {
    {Action::kJump, buttons::pressed::kJump},
    {Action::kReload, buttons::pressed::kReload},
    {Action::kToggleFireMode, buttons::pressed::kFireMode},
    {Action::kDropWeapon, buttons::pressed::kDropWeapon},
    {Action::kInteract, buttons::pressed::kInteract},
    {Action::kZeroUp, buttons::pressed::kZeroUp},
    {Action::kZeroDown, buttons::pressed::kZeroDown},
};

// Returns the slot whose key went down this frame, or -1; the rifle wins if
// both did (like the offline game).
int8_t SelectedSlot(const input::ActionState& actions) {
  if (actions.pressed(Action::kEquipRifle)) {
    return static_cast<int8_t>(weapon::WeaponSlot::kRifle);
  }
  if (actions.pressed(Action::kEquipPistol)) {
    return static_cast<int8_t>(weapon::WeaponSlot::kPistol);
  }
  return -1;
}

}  // namespace

void CommandBuilder::Accumulate(const input::InputFrame& frame,
                                const input::InputSettings& settings,
                                camera::LookAngles look) {
  const input::ActionState& actions = frame.actions;
  uint32_t held = 0;
  for (const ActionBit& entry : kHeldActions) {
    if (actions.held(entry.action)) {
      held |= entry.bit;
    }
    if (actions.pressed(entry.action)) {
      tapped_ |= entry.bit;
    }
  }

  // Stance. In toggle mode the latch follows the request, like the offline
  // player controller.
  if (settings.crouch_toggle) {
    if (actions.pressed(Action::kCrouch)) {
      crouch_latched_ = !crouch_latched_;
    }
    if (crouch_latched_) {
      held |= buttons::held::kCrouch;
    }
  } else {
    if (actions.held(Action::kCrouch)) {
      held |= buttons::held::kCrouch;
    }
    if (actions.pressed(Action::kCrouch)) {
      tapped_ |= buttons::held::kCrouch;
    }
  }

  if (settings.sprint_toggle) {
    if (actions.pressed(Action::kSprint)) {
      sprint_latched_ = !sprint_latched_;
    }
    const player::MoveInput move{
        .forward = actions.held(Action::kMoveForward),
        .back = actions.held(Action::kMoveBack),
        .left = actions.held(Action::kMoveLeft),
        .right = actions.held(Action::kMoveRight),
    };
    if (!player::IsMoving(move) || actions.held(Action::kAim)) {
      // Letting go of the keys or raising the sights ends a sprint.
      sprint_latched_ = false;
    }
    if (sprint_latched_) {
      held |= buttons::held::kSprint;
    }
  } else {
    if (actions.held(Action::kSprint)) {
      held |= buttons::held::kSprint;
    }
    if (actions.pressed(Action::kSprint)) {
      tapped_ |= buttons::held::kSprint;
    }
  }
  held_ = held;

  for (const ActionBit& entry : kPressedActions) {
    if (actions.pressed(entry.action)) {
      pressed_ |= entry.bit;
    }
  }
  if (const int8_t slot = SelectedSlot(actions); slot >= 0) {
    select_slot_ = slot;
  }
  look_ = look;
}

UserCommand CommandBuilder::Take(uint32_t tick) {
  const UserCommand command{
      .tick = tick,
      .held = held_ | tapped_,
      .pressed = pressed_,
      .yaw = look_.yaw,
      .pitch = look_.pitch,
      .select_slot = select_slot_,
      .interact_kind = 0,
      .interact_id = 0,
  };
  tapped_ = 0;
  pressed_ = 0;
  select_slot_ = -1;
  return command;
}

void CommandBuilder::Reset() { *this = CommandBuilder(); }

}  // namespace blue::sim
