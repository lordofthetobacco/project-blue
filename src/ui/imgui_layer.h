#ifndef BLUE_UI_IMGUI_LAYER_H_
#define BLUE_UI_IMGUI_LAYER_H_

#include <SDL3/SDL_events.h>
#include <imgui.h>

#include <string>

#include "platform/window.h"
#include "render/renderer.h"

namespace blue::ui {

// How an ImGuiLayer sizes its UI.
enum class UiScaleMode {
  // Full size at ImGuiLayer::kReferenceHeight logical pixels, times the
  // monitor's DPI scale: the UI keeps the same share of the window when it
  // is resized (the game overlay).
  kWindowHeight,
  // The monitor's DPI scale only: a resized window shows more UI (the
  // editor).
  kDisplayScale,
};

struct ImGuiLayerOptions {
  UiScaleMode scale_mode = UiScaleMode::kWindowHeight;
  // Enables ImGuiConfigFlags_DockingEnable (dockable windows).
  bool docking = false;
  // Where ImGui keeps window positions and dock layout between sessions;
  // empty disables the file.
  std::string ini_path;
  // Lets ImGui change the mouse cursor shape (resize arrows, text beam).
  bool change_mouse_cursor = false;
};

// The Dear ImGui context with its SDL3 platform backend, shared by the game
// overlay and the editor. The renderer owns ImGui's renderer backend
// (render::Renderer::InitImGuiRenderer()) and draws the UI to the window.
//
// Per frame: ProcessEvent() for every SDL event, BeginFrame(), any ImGui
// calls, EndFrame(), then hand its draw data to render::Renderer::DrawFrame()
// or DrawViewportFrame().
//
// There is one ImGui context per process: at most one ImGuiLayer may exist
// at a time. Borrows `window` and `renderer`, which must outlive it. Neither
// copyable nor movable. Not thread-safe.
class ImGuiLayer {
 public:
  // Logical window height at which the UI is drawn at its designed size
  // (UiScaleMode::kWindowHeight).
  static constexpr float kReferenceHeight = 1080.0f;
  static constexpr float kMinUiScale = 0.5f;
  static constexpr float kMaxUiScale = 4.0f;

  // Creates the context, initializes the SDL3 backend and has `renderer`
  // initialize ImGui's renderer backend; failure is fatal.
  ImGuiLayer(const platform::Window& window, const render::Renderer& renderer,
             const ImGuiLayerOptions& options);
  // Waits for the GPU, then shuts the backends down.
  ~ImGuiLayer();

  ImGuiLayer(const ImGuiLayer&) = delete;
  ImGuiLayer& operator=(const ImGuiLayer&) = delete;

  // Forwards an SDL event to ImGui.
  void ProcessEvent(const SDL_Event& event);

  // Starts an ImGui frame: SDL3 backend new frame, scale update,
  // ImGui::NewFrame. (The renderer starts its own backend's frame.)
  void BeginFrame();
  // Ends the frame BeginFrame() started and returns its draw data, valid
  // until the next BeginFrame().
  [[nodiscard]] ImDrawData* EndFrame();

  // Returns the ImGui texture showing the renderer's editor viewport image
  // (render::Renderer::PrepareViewport()); 0 before the first
  // PrepareViewport(). Call between BeginFrame() and EndFrame(), after
  // PrepareViewport().
  [[nodiscard]] ImTextureID ViewportTexture() const;

  // Whether ImGui is using the mouse / keyboard right now (hovering or
  // dragging a widget, typing into a field).
  [[nodiscard]] bool WantsMouse() const;
  [[nodiscard]] bool WantsKeyboard() const;
  // The current UI scale factor (1 = designed size).
  [[nodiscard]] float ui_scale() const { return ui_scale_; }

 private:
  // Recomputes the UI scale from the window and DPI and, if it changed,
  // rebuilds the style around it. Call between frames.
  void UpdateScale();

  const platform::Window& window_;
  const render::Renderer& renderer_;
  ImGuiLayerOptions options_;
  float ui_scale_ = 0.0f;  // 0 until the first UpdateScale().
};

}  // namespace blue::ui

#endif  // BLUE_UI_IMGUI_LAYER_H_
