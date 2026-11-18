#ifndef BLUE_UI_IMGUI_LAYER_H_
#define BLUE_UI_IMGUI_LAYER_H_

#include <SDL3/SDL_events.h>
#include <imgui.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <vector>

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

// The Dear ImGui context with its SDL3 and Vulkan backends, shared by the
// game overlay and the editor. The Vulkan backend draws to the swapchain
// with dynamic rendering and no depth (render::RecordUiPass).
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

  // Creates the context and initializes both backends; failure is fatal.
  ImGuiLayer(const platform::Window& window, const render::Renderer& renderer,
             const ImGuiLayerOptions& options);
  // Waits for the GPU, then shuts the backends down.
  ~ImGuiLayer();

  ImGuiLayer(const ImGuiLayer&) = delete;
  ImGuiLayer& operator=(const ImGuiLayer&) = delete;

  // Forwards an SDL event to ImGui.
  void ProcessEvent(const SDL_Event& event);

  // Starts an ImGui frame: backend new frames, scale update, ImGui::NewFrame.
  void BeginFrame();
  // Ends the frame BeginFrame() started and returns its draw data, valid
  // until the next BeginFrame().
  [[nodiscard]] ImDrawData* EndFrame();

  // Returns the ImGui texture showing the renderer's editor viewport image
  // (render::Renderer::PrepareViewport()), registering it again whenever
  // the renderer recreated the image. Call between BeginFrame() and
  // EndFrame(), after PrepareViewport(). Descriptor sets of replaced images
  // are freed only once no frame in flight can still use them.
  [[nodiscard]] ImTextureID ViewportTexture();

  // Registers `view` (a sampled 2D image in SHADER_READ_ONLY_OPTIMAL that
  // outlives every frame showing it) as an ImGui texture, e.g. a thumbnail.
  [[nodiscard]] ImTextureID AddTexture(VkImageView view);
  // Unregisters a texture from AddTexture(); its descriptor set is freed
  // once no frame in flight can still use it. The image may be released
  // after the same delay (render::Renderer::ReleaseTextures defers too).
  void RemoveTexture(ImTextureID texture);

  // Whether ImGui is using the mouse / keyboard right now (hovering or
  // dragging a widget, typing into a field).
  [[nodiscard]] bool WantsMouse() const;
  [[nodiscard]] bool WantsKeyboard() const;
  // The current UI scale factor (1 = designed size).
  [[nodiscard]] float ui_scale() const { return ui_scale_; }

 private:
  // A texture descriptor set waiting until no frame in flight uses it.
  struct RetiredTexture {
    VkDescriptorSet set = VK_NULL_HANDLE;
    uint64_t free_at_frame = 0;  // frame_count_ from which it can go.
  };

  // Recomputes the UI scale from the window and DPI and, if it changed,
  // rebuilds the style around it. Call between frames.
  void UpdateScale();
  // Frees retired texture sets whose frames have completed.
  void FreeRetiredTextures();

  const platform::Window& window_;
  const render::Renderer& renderer_;
  ImGuiLayerOptions options_;
  float ui_scale_ = 0.0f;  // 0 until the first UpdateScale().
  // Frames begun so far; the clock for retired textures.
  uint64_t frame_count_ = 0;
  // The registered viewport image: its view and descriptor set.
  VkImageView viewport_view_ = VK_NULL_HANDLE;
  VkDescriptorSet viewport_set_ = VK_NULL_HANDLE;
  std::vector<RetiredTexture> retired_textures_;
};

}  // namespace blue::ui

#endif  // BLUE_UI_IMGUI_LAYER_H_
