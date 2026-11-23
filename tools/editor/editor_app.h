#ifndef BLUE_EDITOR_EDITOR_APP_H_
#define BLUE_EDITOR_EDITOR_APP_H_

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_process.h>
#include <imgui.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/result.h"
#include "editor/editor_context.h"
#include "editor/editor_options.h"
#include "editor/editor_settings.h"
#include "editor/mcp/mcp_host.h"
#include "editor/memory_report.h"
#include "editor/panel.h"
#include "physics/physics_runtime.h"
#include "platform/window.h"
#include "render/renderer.h"
#include "render/scene_view.h"
#include "ui/imgui_layer.h"
#include "world/gpu_backend.h"
#include "world/prefab_cache.h"

namespace blue::editor {

// The editor: window, renderer, ImGui with docking, the physics runtime, the
// prefab cache, the documents (EditorContext), the panels and the main loop.
//
// Per frame: SDL events (shell shortcuts), dialog callbacks, queued text
// commands, the MCP server's tool calls, the prefab preview sync; then inside
// the ImGui frame every panel's Update(), the menu bar, the dock space with the
// visible panels of the active mode, the status bar and the modals; finally the
// renderer draws the viewport panel's SceneView into the viewport image under
// the UI. Neither copyable nor movable. Not thread-safe.
class EditorApp {
 public:
  explicit EditorApp(EditorOptions options);
  ~EditorApp();

  EditorApp(const EditorApp&) = delete;
  EditorApp& operator=(const EditorApp&) = delete;

  // Adds a panel (before Run()); see PanelDesc.
  void AddPanel(PanelDesc desc, std::unique_ptr<Panel> panel);
  [[nodiscard]] EditorContext& context() { return *context_; }

  // Opens the start file, queues --exec, and runs until the window closes
  // (asking about unsaved changes first). Returns the process exit code.
  int Run();

 private:
  struct PanelEntry {
    PanelDesc desc;
    std::unique_ptr<Panel> panel;
    bool open = true;
  };

  void RegisterShellCommands();
  void HandleEvent(const SDL_Event& event);
  // Runs a shell shortcut for a key press; returns true if it was one.
  bool HandleShortcut(const SDL_KeyboardEvent& key);
  void Frame(float dt);
  void DrawMenuBar();
  void DrawStatusBar();
  void DrawDockSpace();
  // Docks every panel into its default slot of the dock space `dock_id`.
  void BuildDefaultLayout(ImGuiID dock_id);
  void DrawPanels();
  void DrawModals();
  // Edit > Settings...: the editor settings (the MCP server section).
  void DrawSettingsWindow();
  void DrawMcpSettings();
  // The memory readout (Settings window section and status bar item).
  void DrawMemorySettings();
  // Applies and, with `persist`, saves the undo history budget in MiB.
  void SetHistoryBudget(int megabytes, bool persist);
  // Applies MCP server settings now and, with `persist`, saves them.
  void SetMcpSettings(const McpSettings& settings, bool persist);
  // Moves the selection of the active document to the next (+1) or
  // previous (-1) object: instances in Scene mode, nodes in Prefab mode.
  void SelectAdjacent(int step);
  // Saves the scene and starts the game on it.
  core::Result<void, std::string> Play();
  void OpenWithDialog();
  void AddRecentFile(const std::string& path);
  void LoadRecentFiles();
  void SaveRecentFiles() const;
  // Asks about unsaved documents, then stops the loop.
  void RequestQuit();
  [[nodiscard]] bool HasUnsavedChanges();

  EditorOptions options_;
  platform::Window window_;
  render::Renderer renderer_;
  ui::ImGuiLayer imgui_;
  // Jolt's process-wide state (one per process): collision cooking and
  // cooked-shape restore in the modes need it alive.
  physics::PhysicsRuntime physics_runtime_;
  world::RendererBackend gpu_;
  world::PrefabCache prefab_cache_;
  std::unique_ptr<EditorContext> context_;
  std::vector<PanelEntry> panels_;
  // Rendered when no viewport panel set a view this frame.
  render::SceneView empty_view_;
  std::vector<std::string> recent_files_;
  std::string pref_dir_;
  bool reset_layout_ = false;
  bool running_ = true;
  bool quit_modal_ = false;
  float frame_ms_ = 0.0f;
  // The last game started by Play (not waited for).
  SDL_Process* game_process_ = nullptr;
  // User preferences (editor_settings.cfg in the pref dir) and the values
  // being edited in the Settings window.
  std::string settings_path_;
  EditorSettings settings_;
  McpSettings mcp_draft_;
  bool settings_open_ = false;
  // Opens the Memory section of the Settings window on the next draw.
  bool show_memory_ = false;
  // The memory readout, refreshed every kMemoryRefreshFrames frames.
  MemoryReport memory_;
  uint64_t memory_frame_ = 0;
  int history_budget_draft_ = kDefaultHistoryBudgetMb;
  // Set by the "mcp" command, applied at the start of the next frame.
  std::optional<McpSettings> pending_mcp_;
  // The MCP server; declared after context_, destroyed (and stopped) first.
  std::unique_ptr<McpHost> mcp_;
};

}  // namespace blue::editor

#endif  // BLUE_EDITOR_EDITOR_APP_H_
