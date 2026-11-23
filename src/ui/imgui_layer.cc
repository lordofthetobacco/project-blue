#include "ui/imgui_layer.h"

#include <imgui.h>
#include <imgui_impl_sdl3.h>

#include <algorithm>
#include <cmath>

#include "platform/window.h"
#include "render/renderer.h"

namespace blue::ui {

ImGuiLayer::ImGuiLayer(const platform::Window& window,
                       const render::Renderer& renderer,
                       const ImGuiLayerOptions& options)
    : window_(window), renderer_(renderer), options_(options) {
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();

  ImGuiIO& io = ImGui::GetIO();
  // options_ owns the string for the context's lifetime.
  io.IniFilename =
      options_.ini_path.empty() ? nullptr : options_.ini_path.c_str();
  if (!options_.change_mouse_cursor) {
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
  }
  if (options_.docking) {
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
  }

  UpdateScale();

  switch (window.graphics()) {
    case platform::WindowGraphics::kVulkan:
      ImGui_ImplSDL3_InitForVulkan(window.handle());
      break;
    case platform::WindowGraphics::kMetal:
      ImGui_ImplSDL3_InitForMetal(window.handle());
      break;
    case platform::WindowGraphics::kNone:
      ImGui_ImplSDL3_InitForOther(window.handle());
      break;
  }
  renderer_.InitImGuiRenderer();
}

ImGuiLayer::~ImGuiLayer() {
  renderer_.WaitIdle();
  renderer_.ShutdownImGuiRenderer();
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext();
}

void ImGuiLayer::ProcessEvent(const SDL_Event& event) {
  ImGui_ImplSDL3_ProcessEvent(&event);
}

void ImGuiLayer::BeginFrame() {
  ImGui_ImplSDL3_NewFrame();  // Refreshes io.DisplaySize from the window.
  UpdateScale();
  ImGui::NewFrame();
}

ImDrawData* ImGuiLayer::EndFrame() {
  ImGui::Render();
  return ImGui::GetDrawData();
}

ImTextureID ImGuiLayer::ViewportTexture() const {
  return renderer_.viewport().texture;
}

bool ImGuiLayer::WantsMouse() const {
  return ImGui::GetCurrentContext() != nullptr &&
         ImGui::GetIO().WantCaptureMouse;
}

bool ImGuiLayer::WantsKeyboard() const {
  return ImGui::GetCurrentContext() != nullptr &&
         ImGui::GetIO().WantCaptureKeyboard;
}

void ImGuiLayer::UpdateScale() {
  float scale = window_.DisplayScale();
  if (options_.scale_mode == UiScaleMode::kWindowHeight) {
    // Before the first frame DisplaySize is still zero; fall back to the
    // window's own logical size then.
    float height = ImGui::GetIO().DisplaySize.y;
    if (height <= 0.0f) {
      height = static_cast<float>(window_.height());
    }
    scale *= height > 0.0f ? height / kReferenceHeight : 1.0f;
  }
  scale = std::clamp(scale, kMinUiScale, kMaxUiScale);
  if (std::abs(scale - ui_scale_) < 1.0e-3f) {
    return;
  }
  ui_scale_ = scale;

  // ScaleAllSizes compounds, so always start from a fresh style.
  ImGuiStyle& style = ImGui::GetStyle();
  style = ImGuiStyle();
  ImGui::StyleColorsDark();
  style.ScaleAllSizes(scale);
  style.FontScaleDpi = scale;
  style.WindowRounding = 4.0f * scale;
}

}  // namespace blue::ui
