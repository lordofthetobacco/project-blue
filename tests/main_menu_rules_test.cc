#include "ui/main_menu_rules.h"

#include <doctest/doctest.h>

#include <optional>
#include <string>

namespace blue::ui {
namespace {

MenuRules Rules(GameState state, bool pause_open = false, bool escape = false,
                bool released = false) {
  return NextMenuRules({.state = state,
                        .pause_open = pause_open,
                        .escape_pressed = escape,
                        .cursor_released = released});
}

TEST_CASE("the main menu shows with a free cursor outside play") {
  for (const GameState state : {GameState::kMainMenu, GameState::kConnecting,
                                GameState::kDisconnected}) {
    const MenuRules rules = Rules(state);
    CHECK(rules.main_menu_visible);
    CHECK(rules.cursor_mode);
    CHECK_FALSE(rules.pause_open);
  }
  CHECK_FALSE(Rules(GameState::kOffline).main_menu_visible);
  CHECK_FALSE(Rules(GameState::kOnline).main_menu_visible);
}

TEST_CASE("escape in the menus never quits") {
  CHECK(Rules(GameState::kMainMenu, false, true).escape == EscapeAction::kNone);
  CHECK(Rules(GameState::kMainMenu, false, true).cursor_mode);
  CHECK(Rules(GameState::kConnecting, false, true).escape ==
        EscapeAction::kCancelConnect);
  CHECK(Rules(GameState::kDisconnected, false, true).escape ==
        EscapeAction::kBackToMenu);
}

TEST_CASE("offline keeps the single-player escape and mouse") {
  const MenuRules rules = Rules(GameState::kOffline, false, true, true);
  CHECK(rules.escape == EscapeAction::kNone);
  CHECK_FALSE(rules.cursor_mode);
  CHECK_FALSE(rules.pause_open);
}

TEST_CASE("online escape toggles the pause overlay") {
  MenuRules rules = Rules(GameState::kOnline);
  CHECK_FALSE(rules.pause_open);
  CHECK_FALSE(rules.cursor_mode);
  rules = Rules(GameState::kOnline, false, true);
  CHECK(rules.pause_open);
  CHECK(rules.cursor_mode);
  CHECK(rules.escape == EscapeAction::kOpenPause);
  rules = Rules(GameState::kOnline, true, false);
  CHECK(rules.pause_open);  // Stays open.
  rules = Rules(GameState::kOnline, true, true);
  CHECK_FALSE(rules.pause_open);
  CHECK_FALSE(rules.cursor_mode);
  CHECK(rules.escape == EscapeAction::kClosePause);
}

TEST_CASE("online a lost cursor capture opens the pause overlay") {
  const MenuRules rules = Rules(GameState::kOnline, false, false, true);
  CHECK(rules.pause_open);
  CHECK(rules.cursor_mode);
  CHECK(rules.escape == EscapeAction::kNone);
  // Released while already paused: stays paused.
  CHECK(Rules(GameState::kOnline, true, false, true).pause_open);
}

TEST_CASE("connect requests are checked") {
  CHECK_FALSE(ConnectProblem("127.0.0.1:27015", "alice").has_value());
  CHECK_FALSE(ConnectProblem("  example.org:1 ", " bob ").has_value());
  CHECK_FALSE(ConnectProblem("[::1]:27015", "carol").has_value());
  CHECK(ConnectProblem("127.0.0.1:27015", "  ").has_value());
  CHECK(ConnectProblem("127.0.0.1:27015", std::string(33, 'x')).has_value());
  CHECK(ConnectProblem("", "alice").has_value());
  CHECK(ConnectProblem("127.0.0.1", "alice").has_value());
  CHECK(ConnectProblem(":27015", "alice").has_value());
  CHECK(ConnectProblem("host:0", "alice").has_value());
  CHECK(ConnectProblem("host:65536", "alice").has_value());
  CHECK(ConnectProblem("host:port", "alice").has_value());
  CHECK_FALSE(ConnectProblem("play.example.com:27015", "alice").has_value());
  CHECK(ConnectProblem("::1:27015", "alice").has_value());
  CHECK(ConnectProblem("fe80::1", "alice").has_value());
}

}  // namespace
}  // namespace blue::ui
