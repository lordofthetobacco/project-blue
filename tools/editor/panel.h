#ifndef BLUE_EDITOR_PANEL_H_
#define BLUE_EDITOR_PANEL_H_

#include <imgui.h>

#include <cstdint>
#include <memory>
#include <string>

namespace blue::editor {

class EditorContext;

// Which modes show a panel.
enum class EditorMode : uint8_t {
  kPrefab,
  kScene,
};

// Bit mask of EditorMode values for PanelDesc::modes.
enum PanelModes : uint8_t {
  kPrefabModeOnly = 1u << static_cast<int>(EditorMode::kPrefab),
  kSceneModeOnly = 1u << static_cast<int>(EditorMode::kScene),
  kAllModes = kPrefabModeOnly | kSceneModeOnly,
};

// Where the default layout docks a panel ("View > Reset layout").
enum class DockSlot : uint8_t {
  kCenter,       // The viewport.
  kLeft,         // Hierarchy / outliner, asset browser below.
  kLeftBottom,   // Asset browser.
  kRight,        // Inspector.
  kRightBottom,  // Secondary inspectors (materials, physics, lights).
  kBottom,       // Timeline, log.
};

// How a panel is shown. The shell owns the window: it calls
// ImGui::Begin(name, &open, window_flags) and End() around Panel::Draw().
struct PanelDesc {
  std::string name;  // Window title; unique, stable (it keys the dock ini).
  uint8_t modes = kAllModes;
  DockSlot slot = DockSlot::kRight;
  bool open_by_default = true;
  ImGuiWindowFlags window_flags = ImGuiWindowFlags_None;
  // No window padding (the viewport image fills the window).
  bool no_padding = false;
};

// A dockable editor panel. Panels are UI-rate code: a handful of virtual
// calls per frame is fine here (the no-virtuals rule is for per-draw engine
// code). Panels read and edit the active document through the context;
// every change goes through Document::BeginEdit/Edit so it can be undone.
class Panel {
 public:
  virtual ~Panel() = default;

  // Draws the panel's contents inside its window (only while it is open,
  // its mode is active and it is not collapsed).
  virtual void Draw(EditorContext& context) = 0;

  // Called once per frame before any panel draws, whatever its visibility
  // (e.g. to advance an animation preview). Default: nothing.
  virtual void Update(EditorContext& /*context*/, float /*dt*/) {}

 protected:
  Panel() = default;
  Panel(const Panel&) = default;
  Panel& operator=(const Panel&) = default;
};

}  // namespace blue::editor

#endif  // BLUE_EDITOR_PANEL_H_
