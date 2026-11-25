#include "ui/main_menu_rules.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "core/text.h"

namespace blue::ui {
namespace {

// Longest player name in UTF-8 bytes (net::kNameMaxBytes).
constexpr size_t kNameMaxBytes = 32;

}  // namespace

MenuRules NextMenuRules(const MenuRulesInput& input) {
  MenuRules rules;
  switch (input.state) {
    case GameState::kMainMenu:
      rules.main_menu_visible = true;
      rules.cursor_mode = true;
      return rules;
    case GameState::kConnecting:
      rules.main_menu_visible = true;
      rules.cursor_mode = true;
      if (input.escape_pressed) {
        rules.escape = EscapeAction::kCancelConnect;
      }
      return rules;
    case GameState::kDisconnected:
      rules.main_menu_visible = true;
      rules.cursor_mode = true;
      if (input.escape_pressed) {
        rules.escape = EscapeAction::kBackToMenu;
      }
      return rules;
    case GameState::kOffline:
      return rules;
    case GameState::kOnline:
      if (input.pause_open) {
        rules.pause_open = !input.escape_pressed;
        rules.escape = input.escape_pressed ? EscapeAction::kClosePause
                                            : EscapeAction::kNone;
      } else if (input.escape_pressed || input.cursor_released) {
        rules.pause_open = true;
        rules.escape = input.escape_pressed ? EscapeAction::kOpenPause
                                            : EscapeAction::kNone;
      }
      rules.cursor_mode = rules.pause_open;
      return rules;
  }
  return rules;
}

std::optional<std::string> ConnectProblem(std::string_view address,
                                          std::string_view player_name) {
  const std::string_view name = core::Trim(player_name);
  if (name.empty()) {
    return "Enter a player name.";
  }
  if (name.size() > kNameMaxBytes) {
    return "The player name is too long (32 bytes at most).";
  }
  const std::string_view trimmed = core::Trim(address);
  const size_t colon = trimmed.rfind(':');
  if (trimmed.empty() || colon == std::string_view::npos || colon == 0) {
    return "Enter the server address as host:port.";
  }
  const std::optional<int> port = core::ParseInt(trimmed.substr(colon + 1));
  if (!port || *port < 1 || *port > 65535) {
    return "The port must be a number from 1 to 65535.";
  }
  return std::nullopt;
}

}  // namespace blue::ui
