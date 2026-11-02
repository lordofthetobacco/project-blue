#ifndef BLUE_UI_PERF_OVERLAY_H_
#define BLUE_UI_PERF_OVERLAY_H_

#include <SDL3/SDL_events.h>
#include <imgui.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include <glm/vec3.hpp>

#include "input/input_settings.h"
#include "platform/window.h"
#include "render/renderer.h"

namespace blue::ui {

// Rolling window of frame times with summary statistics. Copyable.
class FrameTimeHistory {
 public:
  static constexpr size_t kCapacity = 240;

  // Adds a sample, evicting the oldest one once the window is full.
  void Push(float milliseconds);

  // Returns the raw ring buffer; the oldest sample is at offset().
  [[nodiscard]] const float* data() const { return samples_.data(); }
  [[nodiscard]] size_t size() const { return count_; }
  [[nodiscard]] size_t offset() const { return count_ < kCapacity ? 0 : next_; }
  // The statistics return 0 for an empty history.
  [[nodiscard]] float average() const {
    return count_ ? sum_ / static_cast<float>(count_) : 0.0f;
  }
  [[nodiscard]] float min() const;
  [[nodiscard]] float max() const;

 private:
  std::array<float, kCapacity> samples_{};
  size_t next_ = 0;
  size_t count_ = 0;
  float sum_ = 0.0f;
};

// Plain data the overlay shows about the camera; the views must outlive the
// PerfOverlay::Build() call.
struct CameraOverlayInfo {
  std::string_view mode;
  glm::vec3 position{0.0f};
  float yaw_degrees = 0.0f;
  float pitch_degrees = 0.0f;
  float speed = 0.0f;  // Player: horizontal speed. Fly: configured speed.
  float fov_degrees = 0.0f;
  float sensitivity = 0.0f;
  bool invert_y = false;
  std::string_view state;  // Player stance, e.g. "grounded, sprinting".
  std::string_view hint;
};

// Plain data the overlay shows about the physics simulation.
struct PhysicsOverlayInfo {
  int bodies = 0;
  int active_bodies = 0;
  float step_ms = 0.0f;
  glm::vec3 gravity{0.0f};
};

// Plain data the overlay shows about the sun shadows.
struct ShadowOverlayInfo {
  bool enabled = false;  // Shadows are actually being rendered.
  bool cascade_debug = false;
  // GPU time of the shadow and main passes; nullopt when not measured.
  std::optional<double> shadow_ms;
  std::optional<double> main_ms;
};

// Plain data the overlay shows about the loaded scene; the views must outlive
// the PerfOverlay::Build() call.
struct SceneOverlayInfo {
  CameraOverlayInfo camera;
  PhysicsOverlayInfo physics;
  ShadowOverlayInfo shadows;
  // Draws a dot at the screen center, also while the stats window is hidden.
  bool show_crosshair = false;
  size_t entity_count = 0;
  uint32_t draw_calls = 0;
  uint32_t culled_draws = 0;  // Skipped by frustum culling in the main pass.
  uint32_t shadow_draw_calls = 0;
  uint32_t shadow_culled_draws = 0;
  uint64_t triangles = 0;
  std::string_view scene_path;
  std::string_view status;
  // Compiler message of the shader that failed to reload; empty when all
  // shaders are fine. Only its first line is shown, in red.
  std::string_view shader_error;
  // One line listing the current key bindings.
  std::string_view help;
};

// What one Build() call produced.
struct BuildResult {
  ImDrawData* draw_data = nullptr;
  // The user finished changing a value in the settings window this frame.
  bool settings_changed = false;
};

// Dear ImGui performance overlay: FPS, CPU/GPU frame time, frame-time graph,
// device info, player settings. Owns the ImGui context and backends. Borrows
// `window` and `renderer`, which must outlive it. The UI scales with the
// window: full size at kReferenceHeight logical pixels, times the monitor's
// DPI scale, so it keeps the same share of the screen when the window is
// resized or moved between displays. Neither copyable nor movable. Not
// thread-safe.
class PerfOverlay {
 public:
  // Logical window height at which the UI is drawn at its designed size.
  static constexpr float kReferenceHeight = 1080.0f;
  static constexpr float kMinUiScale = 0.5f;
  static constexpr float kMaxUiScale = 4.0f;

  PerfOverlay(const platform::Window& window, const render::Renderer& renderer);
  ~PerfOverlay();

  PerfOverlay(const PerfOverlay&) = delete;
  PerfOverlay& operator=(const PerfOverlay&) = delete;

  // Forwards an SDL event to ImGui.
  void ProcessEvent(const SDL_Event& event);
  void ToggleVisible() { visible_ = !visible_; }

  // Records this frame's timings and builds the UI: the stats and settings
  // windows while visible, plus the crosshair if requested. Edits in the
  // settings window are written straight into `settings`. Always returns the
  // frame's draw data.
  [[nodiscard]] BuildResult Build(float cpu_frame_ms,
                                  std::optional<double> gpu_frame_ms,
                                  const SceneOverlayInfo& scene_info,
                                  input::InputSettings& settings);

  // Whether ImGui is using the mouse / keyboard right now (hovering or
  // dragging a widget). The app keeps such events away from the game.
  [[nodiscard]] bool WantsMouse() const;
  [[nodiscard]] bool WantsKeyboard() const;

 private:
  // Recomputes the UI scale from the window and DPI and, if it changed,
  // rebuilds the style around it. Call between frames.
  void UpdateScale();
  // Each Draw* helper emits one section of the overlay window.
  void DrawTimings(float cpu_frame_ms) const;
  void DrawDeviceInfo() const;
  // Emits the settings window; returns true when an edit was completed.
  [[nodiscard]] bool DrawSettings(input::InputSettings& settings) const;

  const platform::Window& window_;
  const render::Renderer& renderer_;
  FrameTimeHistory cpu_history_;
  std::optional<double> gpu_frame_ms_;
  float ui_scale_ = 0.0f;  // 0 until the first UpdateScale().
  bool visible_ = true;
};

}  // namespace blue::ui

#endif  // BLUE_UI_PERF_OVERLAY_H_
