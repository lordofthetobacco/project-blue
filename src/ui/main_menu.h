#ifndef BLUE_UI_MAIN_MENU_H_
#define BLUE_UI_MAIN_MENU_H_

#include <span>
#include <string_view>

#include "ui/main_menu_rules.h"

// The game's main menu (connect to a server or play offline) and the
// in-game pause overlay of a networked game: ImGui windows centred on the
// display. They show what they are given and report clicks; the rules are
// in ui/main_menu_rules.h.

namespace blue::ui {

// What the main menu shows this frame. The text buffers are edited in
// place (NUL-terminated); every view must outlive the DrawMainMenu() call.
struct MainMenuInfo {
  GameState state = GameState::kMainMenu;
  std::span<char> address;      // "host:port"
  std::span<char> player_name;  // UTF-8
  // Progress ("Connecting to ..."), an error, or the disconnect reason;
  // empty for none.
  std::string_view status;
  bool status_is_error = false;
};

// What the player did on the main menu this frame.
struct MainMenuResult {
  bool connect = false;       // Connect (or Enter in a field)
  bool play_offline = false;  // Play offline
  bool quit = false;          // Quit
  bool cancel = false;        // Cancel while connecting
  bool back = false;          // Back after a disconnect
};

// Emits the main menu into the current ImGui frame (after
// PerfOverlay::Build, before Render). The fields are disabled while
// connecting.
[[nodiscard]] MainMenuResult DrawMainMenu(const MainMenuInfo& info);

// What the pause overlay shows.
struct PauseMenuInfo {
  std::string_view server;  // the address played on
};

// What the player did on the pause overlay this frame.
struct PauseMenuResult {
  bool resume = false;
  bool disconnect = false;
  bool quit = false;
};

// Emits the pause overlay of a networked game into the current ImGui frame.
[[nodiscard]] PauseMenuResult DrawPauseMenu(const PauseMenuInfo& info);

}  // namespace blue::ui

#endif  // BLUE_UI_MAIN_MENU_H_
