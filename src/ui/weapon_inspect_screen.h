#ifndef BLUE_UI_WEAPON_INSPECT_SCREEN_H_
#define BLUE_UI_WEAPON_INSPECT_SCREEN_H_

#include <optional>
#include <span>
#include <string_view>

#include "game/weapon_modding.h"

// The weapon inspect / modding screen: ImGui panels over the inspected
// weapon. Shows what game::WeaponModding prepared and reports what the
// player clicked or hovers; it decides nothing itself.

namespace blue::ui {

// What the screen shows this frame. Every view must outlive the
// DrawInspectScreen() call.
struct InspectScreenInfo {
  std::string_view weapon_name;
  // parts::MissingSummary() of the stats; empty for a functional weapon.
  std::string_view missing;
  std::span<const game::SlotRow> rows;
  int selected_row = -1;  // -1: none
  bool can_remove = false;
  // Stats table rows (game::MakeStatLines), with a preview when
  // `has_preview`.
  std::span<const game::StatLine> stats;
  bool has_preview = false;
  // The stash entries that fit the selected slot.
  std::span<const game::StashLine> stash;
  // Why the last change failed, or a hint; empty for none.
  std::string_view message;
  // True when the weapon is not built from parts (no catalogue or a
  // single-piece weapon): only the name and stats are shown.
  bool no_parts = false;
};

// What the player did on the screen this frame.
struct InspectScreenResult {
  std::optional<int> select_row;  // a slot row was clicked
  std::optional<int> install;     // a stash line was clicked (its index)
  bool remove = false;            // Remove was clicked
  // What the pointer is on, for next frame's preview.
  std::optional<int> hovered_stash;  // a stash line index
  bool remove_hovered = false;
};

// Emits the screen's panels into the current ImGui frame (after
// PerfOverlay::Build, before Render), anchored to the right side of the
// display and sized with the UI scale (the font size).
[[nodiscard]] InspectScreenResult DrawInspectScreen(
    const InspectScreenInfo& info);

}  // namespace blue::ui

#endif  // BLUE_UI_WEAPON_INSPECT_SCREEN_H_
