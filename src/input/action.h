#ifndef BLUE_INPUT_ACTION_H_
#define BLUE_INPUT_ACTION_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace blue::input {

// Everything the player or the app can be asked to do through a key or mouse
// button. Any action may be bound to any key; a key may drive several actions
// (by default Space is both kJump and kFlyUp, Left Ctrl both kCrouch and
// kFlyDown) and the consumer picks the ones that apply to its mode.
//
// Held actions are meaningful as long as the key is down; the others are
// edge-triggered and consumers look at ActionState::pressed().
enum class Action : uint8_t {
  // Held.
  kMoveForward,
  kMoveBack,
  kMoveLeft,
  kMoveRight,
  kFlyUp,    // Debug fly camera only.
  kFlyDown,  // Debug fly camera only.
  kJump,     // Player only.
  kSprint,   // Player only.
  kCrouch,   // Player only.
  kFire,     // Player only: the trigger.
  kAim,      // Player only: aim down the sights.
  // Edge-triggered.
  kCycleCamera,  // Player -> Fly -> Orbit -> Player.
  kEscape,       // Releases the mouse, or quits when it is already free.
  kToggleInvertY,
  kSensitivityDown,
  kSensitivityUp,
  kToggleOverlay,
  kSaveScene,
  kLoadScene,
  kToggleVsync,
  kPush,  // Push the object in the middle of the view.
  kToggleCascadeDebug,
  kToggleShadows,
  kReload,          // Player only.
  kToggleFireMode,  // Player only: semi <-> auto.
  kEquipPistol,     // Player only: take out the pistol.
  kEquipRifle,      // Player only: take out the rifle.
  kCount,
};

inline constexpr size_t kActionCount = static_cast<size_t>(Action::kCount);

// Returns every action in declaration order.
[[nodiscard]] constexpr std::array<Action, kActionCount> AllActions() {
  std::array<Action, kActionCount> actions{};
  for (size_t i = 0; i < kActionCount; ++i) {
    actions[i] = static_cast<Action>(i);
  }
  return actions;
}

// Returns the stable config name of `action`, e.g. "move_forward". It is the
// suffix of the "bind.<name>" key in the settings file.
[[nodiscard]] std::string_view ActionName(Action action);

// Returns the action called `name` (exact, case-sensitive), or nullopt.
[[nodiscard]] std::optional<Action> ParseAction(std::string_view name);

// Returns true for actions whose press is what matters, false for the held
// movement and stance actions.
[[nodiscard]] bool IsEdgeTriggered(Action action);

}  // namespace blue::input

#endif  // BLUE_INPUT_ACTION_H_
