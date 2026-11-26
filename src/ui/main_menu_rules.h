#ifndef BLUE_UI_MAIN_MENU_RULES_H_
#define BLUE_UI_MAIN_MENU_RULES_H_

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// The rules of the game's main menu and in-game pause overlay: which is
// shown, whether the cursor is free, what Escape does, and which connect
// requests are acceptable. Pure functions; the ImGui side is
// ui/main_menu.h.

namespace blue::ui {

// What the game is doing.
enum class GameState : uint8_t {
  kMainMenu,      // The menu, no game running.
  kOffline,       // Single-player, as without the menu.
  kConnecting,    // Connecting to a server; the menu shows the progress.
  kOnline,        // Playing on a server.
  kDisconnected,  // A connection ended; the menu shows why.
};

// What Escape asks for this frame.
enum class EscapeAction : uint8_t {
  kNone,
  kOpenPause,
  kClosePause,
  kCancelConnect,  // While connecting: give up.
  kBackToMenu,     // After a disconnect: acknowledge, back to the menu.
};

// What decides the menu for a frame.
struct MenuRulesInput {
  GameState state = GameState::kMainMenu;
  bool pause_open = false;      // the pause overlay was open last frame
  bool escape_pressed = false;  // kEscape this frame
  // The cursor is free although the game wanted it captured (the window
  // lost the focus, or the capture was released).
  bool cursor_released = false;
};

// The menu for this frame.
struct MenuRules {
  bool main_menu_visible = false;
  bool pause_open = false;
  // The cursor is used for widgets (InputMapper::MouseMode::kCursor), so
  // Escape reaches the game instead of quitting. False: the game's own mouse
  // mode applies.
  bool cursor_mode = false;
  EscapeAction escape = EscapeAction::kNone;
};

// Returns the menu for the frame:
//   - the main menu shows in kMainMenu, kConnecting and kDisconnected, in
//     cursor mode; Escape there cancels a connection attempt or leaves the
//     disconnect message, and otherwise does nothing (Quit is a button);
//   - offline there is no menu and Escape keeps its single-player meaning
//     (release the mouse, then quit);
//   - online, Escape toggles the pause overlay (cursor mode while open), and
//     a cursor released some other way opens it too, so Escape never quits
//     a networked game.
[[nodiscard]] MenuRules NextMenuRules(const MenuRulesInput& input);

// Returns why `address` ("host:port") and `player_name` cannot be used to
// connect, or nullopt when they can. The name is judged after
// net::SanitizePlayerName-like trimming by the caller.
[[nodiscard]] std::optional<std::string> ConnectProblem(
    std::string_view address, std::string_view player_name);

}  // namespace blue::ui

#endif  // BLUE_UI_MAIN_MENU_RULES_H_
