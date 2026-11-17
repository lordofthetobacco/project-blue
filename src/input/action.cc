#include "input/action.h"

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>

namespace blue::input {
namespace {

// Indexed by Action; the order must match the enum.
constexpr std::array<std::string_view, kActionCount> kActionNames{
    "move_forward",   "move_back",        "move_left",
    "move_right",     "fly_up",           "fly_down",
    "jump",           "sprint",           "crouch",
    "fire",           "aim",
    "cycle_camera",   "escape",           "toggle_invert_y",
    "sensitivity_down", "sensitivity_up", "toggle_overlay",
    "save_scene",     "load_scene",       "toggle_vsync",
    "push",           "toggle_cascade_debug", "toggle_shadows",
    "reload",         "toggle_fire_mode", "equip_pistol",
    "equip_rifle",
};

}  // namespace

std::string_view ActionName(Action action) {
  const size_t index = static_cast<size_t>(action);
  return index < kActionCount ? kActionNames[index] : std::string_view{};
}

std::optional<Action> ParseAction(std::string_view name) {
  for (size_t i = 0; i < kActionCount; ++i) {
    if (kActionNames[i] == name) {
      return static_cast<Action>(i);
    }
  }
  return std::nullopt;
}

bool IsEdgeTriggered(Action action) {
  return action >= Action::kCycleCamera && action < Action::kCount;
}

}  // namespace blue::input
