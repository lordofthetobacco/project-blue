#ifndef BLUE_UI_PERF_OVERLAY_H_
#define BLUE_UI_PERF_OVERLAY_H_

#include <SDL3/SDL_events.h>
#include <imgui.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include "input/input_settings.h"
#include "platform/window.h"
#include "render/renderer.h"
#include "render/scene_view.h"
#include "ui/imgui_layer.h"
#include "weapon/target_log.h"

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
};

// Plain data the overlay shows about the weapon in hand; the views must
// outlive the PerfOverlay::Build() call.
struct WeaponOverlayInfo {
  // No weapon in hand: only the bullet counts below are meaningful.
  bool hands_empty = false;
  std::string_view name;  // The weapon in hand, e.g. "Glock 17".
  // A part-built weapon's stats, e.g. "0.91 kg  ergo 91  4.4 MOA  50 m
  // recoil 302/252  1100 rpm"; empty for a single-piece weapon.
  std::string_view stats;
  // "NOT FUNCTIONAL: missing <slots>" for a weapon missing vital parts;
  // empty otherwise.
  std::string_view not_functional;
  // Where a weapon switch in progress is heading; empty when not switching.
  std::string_view switching_to;
  int rounds_in_magazine = 0;
  bool magazine_inserted = true;
  bool round_chambered = false;
  bool bolt_locked_back = false;
  bool reloading = false;
  float reload_fraction = 0.0f;  // [0, 1]
  std::string_view fire_mode;    // e.g. "SEMI", "AUTO", "DA"
  // The chosen scope magnification of a scoped weapon in hand (shown as
  // "scope 6.0x"); 0 otherwise.
  float scope_magnification = 0.0f;
  // The sights' zero in metres (shown as "zero 200 m"); 0 for a weapon
  // whose zero cannot be set.
  float scope_zero = 0.0f;
  std::span<const int> spare_magazines;  // Round count of each spare.
  int projectiles = 0;                   // Bullets in flight.
  int impacts = 0;                       // Hits since the last scene reload.
};

// What the hit log window shows; everything is borrowed for the
// PerfOverlay::Build() call. The window is hidden while `log` is null.
struct HitLogOverlayInfo {
  const weapon::TargetLog* log = nullptr;
  // Name of each target index of `log`; indices past the end show as "?".
  std::span<const std::string> target_names;
  int shots_fired = 0;
};

// A name drawn over a remote player at `screen` (display pixels, origin top
// left), projected by the game.
struct NameTag {
  glm::vec2 screen{0.0f};
  std::string_view name;
};

// One kill feed line, e.g. "alice > bob", fading out with `opacity`.
struct KillFeedLine {
  std::string_view text;
  float opacity = 1.0f;  // [0, 1]
};

// What a networked game adds to the overlay: the HUD (health, ammo), a net
// quality line, name tags and the kill feed. Everything is borrowed for the
// PerfOverlay::Build() call.
struct NetHudInfo {
  int health = 0;
  bool alive = true;
  float respawn_seconds = 0.0f;  // while dead
  bool has_weapon = false;
  int rounds_in_magazine = 0;
  int spare_rounds = 0;  // in the spare magazines
  double ping_ms = 0.0;
  double loss_percent = 0.0;
  double snapshots_per_second = 0.0;
  double replays_per_second = 0.0;
  double interpolation_ms = 0.0;
  std::span<const NameTag> name_tags;
  std::span<const KillFeedLine> kill_feed;
};

// Plain data the overlay shows about the loaded scene; the views must outlive
// the PerfOverlay::Build() call.
struct SceneOverlayInfo {
  CameraOverlayInfo camera;
  PhysicsOverlayInfo physics;
  ShadowOverlayInfo shadows;
  WeaponOverlayInfo weapon;
  HitLogOverlayInfo hit_log;
  // Draws a dot at the screen center, also while the stats window is hidden.
  bool show_crosshair = false;
  // Opacity in [0, 1] of the interaction dot at the screen center (the
  // player looks at something they can use); 0 hides it. Also drawn while
  // the stats window is hidden.
  float interaction_dot_opacity = 0.0f;
  size_t entity_count = 0;
  uint32_t draw_calls = 0;
  uint32_t culled_draws = 0;  // Skipped by frustum culling in the main pass.
  uint32_t shadow_draw_calls = 0;
  uint32_t shadow_culled_draws = 0;
  uint64_t triangles = 0;
  // Shown as is: pass the scene's file name (the full path is in the log)
  // so the window stays narrow.
  std::string_view scene_name;
  std::string_view status;
  // Compiler message of the shader that failed to reload; empty when all
  // shaders are fine. Only its first line is shown, in red.
  std::string_view shader_error;
  // One line listing the current key bindings.
  std::string_view help;
  // The weapon inspect screen is open: the stats, settings and hit log
  // windows stay hidden so its panels have the screen.
  bool hide_windows = false;
  // Online only: the HUD, net line, name tags and kill feed (drawn also
  // while the stats window is hidden); null offline.
  const NetHudInfo* net = nullptr;
};

// What one Build() call produced.
struct BuildResult {
  // The user finished changing a value in the settings window this frame.
  bool settings_changed = false;
  // The user pressed Clear in the hit log window this frame.
  bool clear_hit_log = false;
};

// Dear ImGui performance overlay: FPS, CPU/GPU frame time, frame-time graph,
// device info, player settings. Owns the ImGui layer (context and backends).
// Borrows `window` and `renderer`, which must outlive it. The UI scales
// with the window (UiScaleMode::kWindowHeight), so it keeps the same share of
// the screen when the window is resized or moved between displays. Neither
// copyable nor movable. Not thread-safe.
class PerfOverlay {
 public:
  PerfOverlay(const platform::Window& window, const render::Renderer& renderer);

  PerfOverlay(const PerfOverlay&) = delete;
  PerfOverlay& operator=(const PerfOverlay&) = delete;

  // Forwards an SDL event to ImGui.
  void ProcessEvent(const SDL_Event& event) { layer_.ProcessEvent(event); }
  void ToggleVisible() { visible_ = !visible_; }
  [[nodiscard]] bool visible() const { return visible_; }

  // Records this frame's timings, starts the ImGui frame and builds the
  // overlay: the stats and settings windows while visible, plus the
  // crosshair if requested. Edits in the settings window are written
  // straight into `settings` and `post` (exposure and post-processing,
  // applied live and not saved). More windows may be emitted after it;
  // Render() ends the frame.
  [[nodiscard]] BuildResult Build(float cpu_frame_ms,
                                  std::optional<double> gpu_frame_ms,
                                  const SceneOverlayInfo& scene_info,
                                  input::InputSettings& settings,
                                  render::PostSettings& post);
  // Ends the ImGui frame Build() started and returns its draw data.
  [[nodiscard]] ImDrawData* Render() { return layer_.EndFrame(); }

  // Whether ImGui is using the mouse / keyboard right now (hovering or
  // dragging a widget). The app keeps such events away from the game.
  [[nodiscard]] bool WantsMouse() const { return layer_.WantsMouse(); }
  [[nodiscard]] bool WantsKeyboard() const { return layer_.WantsKeyboard(); }

 private:
  // Each Draw* helper emits one section of the overlay window.
  void DrawTimings(float cpu_frame_ms) const;
  void DrawDeviceInfo() const;
  // Emits the settings window; returns true when an edit of `settings` was
  // completed. Edits of `post` apply at once.
  [[nodiscard]] bool DrawSettings(input::InputSettings& settings,
                                  render::PostSettings& post) const;
  // Emits the hit log window; returns true when Clear was pressed.
  [[nodiscard]] bool DrawHitLog(const HitLogOverlayInfo& info) const;

  const render::Renderer& renderer_;
  ImGuiLayer layer_;
  FrameTimeHistory cpu_history_;
  std::optional<double> gpu_frame_ms_;
  bool visible_ = true;
};

}  // namespace blue::ui

#endif  // BLUE_UI_PERF_OVERLAY_H_
