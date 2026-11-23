#include "ui/perf_overlay.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <glm/vec2.hpp>

#include "core/extent.h"
#include "lighting/cascades.h"
#include "platform/window.h"
#include "render/gpu_scope.h"
#include "render/renderer.h"
#include "render/scene_view.h"
#include "render/temporal_math.h"
#include "ui/imgui_layer.h"
#include "weapon/target_log.h"

namespace blue::ui {

namespace {

constexpr ImVec4 kShaderErrorColor{1.0f, 0.3f, 0.3f, 1.0f};

// Shows `text` as a plain label; ImGui::Text would treat it as a format
// string and string views are not null-terminated.
void TextView(std::string_view text) {
  ImGui::TextUnformatted(text.data(), text.data() + text.size());
}

void DrawCameraInfo(const CameraOverlayInfo& camera) {
  ImGui::Text("Camera: %.*s", static_cast<int>(camera.mode.size()),
              camera.mode.data());
  ImGui::Text("  pos %.1f %.1f %.1f", camera.position.x, camera.position.y,
              camera.position.z);
  ImGui::Text("  yaw %.1f  pitch %.1f deg", camera.yaw_degrees,
              camera.pitch_degrees);
  ImGui::Text("  speed %.1f m/s  fov %.0f deg", camera.speed,
              camera.fov_degrees);
  ImGui::Text("  sens %.4f rad/count  invert Y %s", camera.sensitivity,
              camera.invert_y ? "on" : "off");
  if (!camera.state.empty()) {
    ImGui::Text("  %.*s", static_cast<int>(camera.state.size()),
                camera.state.data());
  }
  ImGui::TextDisabled("%.*s", static_cast<int>(camera.hint.size()),
                      camera.hint.data());
}

void DrawPhysicsInfo(const PhysicsOverlayInfo& physics) {
  ImGui::Text("Physics: %d bodies, %d active", physics.bodies,
              physics.active_bodies);
  ImGui::Text("  step %.3f ms  gravity %.2f %.2f %.2f", physics.step_ms,
              physics.gravity.x, physics.gravity.y, physics.gravity.z);
}

void DrawShadowInfo(const ShadowOverlayInfo& shadows) {
  ImGui::Text("Shadows: %s  (%d x %dpx)", shadows.enabled ? "on" : "off",
              lighting::kCascadeCount, lighting::kShadowMapSize);
  ImGui::Text("  cascade debug: %s", shadows.cascade_debug ? "on" : "off");
}

// Shows the weapon in hand and its ammunition state, e.g.
// "M16A2: 30+1  SEMI  mags 30/30/30/30".
void DrawWeaponInfo(const WeaponOverlayInfo& weapon) {
  if (weapon.hands_empty) {
    ImGui::TextUnformatted("Empty hands");
    ImGui::Text("  bullets in flight %d  hits %d", weapon.projectiles,
                weapon.impacts);
    return;
  }
  // Spare magazine counts, formatted into a fixed buffer: no allocation.
  std::array<char, 96> spares{};
  size_t used = 0;
  for (const int rounds : weapon.spare_magazines) {
    const size_t room = spares.size() - 1 - used;
    const auto written = std::format_to_n(spares.data() + used, room, "{}{}",
                                          used == 0 ? "" : "/", rounds);
    used += std::min(static_cast<size_t>(written.size), room);
  }
  const std::string_view spare_text =
      used == 0 ? std::string_view("none")
                : std::string_view(spares.data(), used);

  ImGui::Text("%.*s: %d+%d  %.*s  mags %.*s",
              static_cast<int>(weapon.name.size()), weapon.name.data(),
              weapon.rounds_in_magazine, weapon.round_chambered ? 1 : 0,
              static_cast<int>(weapon.fire_mode.size()),
              weapon.fire_mode.data(), static_cast<int>(spare_text.size()),
              spare_text.data());
  if (!weapon.not_functional.empty()) {
    ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f), "  %.*s",
                       static_cast<int>(weapon.not_functional.size()),
                       weapon.not_functional.data());
  }
  if (!weapon.stats.empty()) {
    ImGui::Text("  %.*s", static_cast<int>(weapon.stats.size()),
                weapon.stats.data());
  }
  if (weapon.scope_magnification > 0.0f) {
    ImGui::Text("  scope %.1fx  zero %.0f m", weapon.scope_magnification,
                weapon.scope_zero);
  } else if (weapon.scope_zero > 0.0f) {
    ImGui::Text("  zero %.0f m", weapon.scope_zero);
  }
  if (!weapon.switching_to.empty()) {
    ImGui::Text("  switching to %.*s",
                static_cast<int>(weapon.switching_to.size()),
                weapon.switching_to.data());
  } else if (weapon.reloading) {
    ImGui::Text("  reloading %.0f%%%s", weapon.reload_fraction * 100.0f,
                weapon.magazine_inserted ? "" : "  (magazine out)");
  } else if (weapon.bolt_locked_back) {
    ImGui::TextUnformatted("  locked open: empty");
  } else if (!weapon.round_chambered) {
    ImGui::TextUnformatted("  chamber empty");
  }
  ImGui::Text("  bullets in flight %d  hits %d", weapon.projectiles,
              weapon.impacts);
}

// A hit-log line as wide as the widest ordinary first line of DrawHit, for
// sizing the log so nothing is cut off.
constexpr const char* kWidestHitLine =
    "#9999 target 600 m     R +999.9 U +999.9 cm  (+99.99 +99.99 mrad, "
    "+999.9 +999.9 MOA)";
// Hit-log lines shown before the region scrolls; each hit takes two.
constexpr float kHitLogVisibleLines = 16.0f;

// Shows one hit on two lines: shot, target and offset in cm and as angles at
// the target's range; then miss distance, flight time, impact speed and
// energy.
void DrawHit(const weapon::TargetHit& hit,
             std::span<const std::string> target_names) {
  constexpr float kMilliradiansPerRadian = 1000.0f;
  constexpr float kMoaPerRadian = 3437.747f;  // 60 * 180 / pi
  const size_t index = static_cast<size_t>(hit.target);
  const char* name = hit.target >= 0 && index < target_names.size()
                         ? target_names[index].c_str()
                         : "?";
  const glm::vec2 angle =
      hit.range_m > 0.0f ? hit.offset_m / hit.range_m : glm::vec2(0.0f);
  const glm::vec2 offset_cm = hit.offset_m * 100.0f;
  const glm::vec2 mrad = angle * kMilliradiansPerRadian;
  const glm::vec2 moa = angle * kMoaPerRadian;
  ImGui::Text(
      "#%-4d %-14s R %+6.1f U %+6.1f cm  (%+5.2f %+5.2f mrad, %+5.1f %+5.1f "
      "MOA)",
      hit.shot_number, name, offset_cm.x, offset_cm.y, mrad.x, mrad.y, moa.x,
      moa.y);
  ImGui::TextDisabled("      miss %5.1f cm  %4.0f ms  %4.0f m/s  %5.0f J%s",
                      hit.miss_m * 100.0f, hit.flight_time * 1000.0f,
                      hit.impact_speed, hit.energy_j,
                      hit.on_plate ? "" : "  edge");
}

// Draws a small dot at the center of the screen behind all windows.
void DrawCrosshair() {
  const ImVec2 size = ImGui::GetIO().DisplaySize;
  const float radius = 2.5f * ImGui::GetStyle().FontScaleDpi;
  ImDrawList* draw_list = ImGui::GetBackgroundDrawList();
  const ImVec2 center{size.x * 0.5f, size.y * 0.5f};
  draw_list->AddCircleFilled(center, radius + 1.0f, IM_COL32(0, 0, 0, 160));
  draw_list->AddCircleFilled(center, radius, IM_COL32(255, 255, 255, 230));
}

// Draws the interaction dot at the center of the screen behind all windows:
// about 3 px across at 1080p (scaled with the UI), white with a thin dark
// rim so it reads on any background, at `opacity` in (0, 1].
void DrawInteractionDot(float opacity) {
  const ImVec2 size = ImGui::GetIO().DisplaySize;
  const float scale = ImGui::GetStyle().FontScaleDpi;
  const float radius = 1.5f * scale;
  const float rim = std::max(0.75f * scale, 1.0f);
  const float alpha = std::clamp(opacity, 0.0f, 1.0f);
  ImDrawList* draw_list = ImGui::GetBackgroundDrawList();
  const ImVec2 center{size.x * 0.5f, size.y * 0.5f};
  draw_list->AddCircleFilled(
      center, radius + rim,
      ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, 0.6f * alpha)));
  draw_list->AddCircleFilled(
      center, radius, ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, alpha)));
}

// Emits the temporal upscaling mode and FXAA, which is shown inactive while
// the temporal scaler does the anti-aliasing.
void DrawAntiAliasingSettings(render::PostSettings& post) {
  ImGui::PushItemWidth(ImGui::GetFontSize() * 10.0f);
  if (ImGui::BeginCombo("Upscaling", render::UpscalingName(post.upscaling))) {
    for (int32_t i = 0; i < render::kUpscalingModeCount; ++i) {
      const auto mode = static_cast<render::Upscaling>(i);
      const bool selected = mode == post.upscaling;
      if (ImGui::Selectable(render::UpscalingName(mode), selected)) {
        post.upscaling = mode;
      }
      if (selected) {
        ImGui::SetItemDefaultFocus();
      }
    }
    ImGui::EndCombo();
  }
  ImGui::PopItemWidth();
  const bool temporal = render::UsesTemporalScaler(post.upscaling);
  ImGui::BeginDisabled(temporal);
  // One widget whichever label it shows (the id follows ###).
  ImGui::Checkbox(temporal ? "FXAA (replaced by temporal AA)###fxaa"
                           : "FXAA###fxaa",
                  &post.fxaa_enabled);
  ImGui::EndDisabled();
}

// Emits the exposure and post-processing controls (applied live, reset by a
// scene reload).
void DrawPostSettings(render::PostSettings& post) {
  if (!ImGui::CollapsingHeader("Image")) {
    return;
  }
  constexpr float kMaxExposureEv = 8.0f;
  constexpr float kMaxBloomStrength = 0.3f;
  constexpr float kMaxMotionBlurStrength = 2.0f;
  constexpr const char* kTonemappers[] = {"ACES fitted", "PBR Neutral",
                                          "Clamp"};
  ImGui::PushItemWidth(ImGui::GetFontSize() * 10.0f);
  ImGui::SliderFloat("Exposure (EV)", &post.exposure_ev, -kMaxExposureEv,
                     kMaxExposureEv, "%.2f");
  int tonemapper = static_cast<int>(post.tonemapper);
  if (ImGui::Combo("Tonemapper", &tonemapper, kTonemappers,
                   static_cast<int>(std::size(kTonemappers)))) {
    post.tonemapper = static_cast<render::Tonemapper>(tonemapper);
  }
  ImGui::Checkbox("Bloom", &post.bloom_enabled);
  ImGui::SliderFloat("Bloom strength", &post.bloom_strength, 0.0f,
                     kMaxBloomStrength, "%.3f");
  ImGui::PopItemWidth();
  ImGui::Checkbox("SSAO", &post.ssao_enabled);
  DrawAntiAliasingSettings(post);
  ImGui::Checkbox("Weapon motion blur", &post.weapon_motion_blur_enabled);
  ImGui::PushItemWidth(ImGui::GetFontSize() * 10.0f);
  ImGui::SliderFloat("Blur strength", &post.weapon_motion_blur_strength, 0.0f,
                     kMaxMotionBlurStrength, "%.2f");
  ImGui::PopItemWidth();
}

void DrawSceneInfo(const SceneOverlayInfo& scene_info) {
  ImGui::Text("Entities %zu  |  %u draws (%u culled)  |  %llu tris",
              scene_info.entity_count, scene_info.draw_calls,
              scene_info.culled_draws,
              static_cast<unsigned long long>(scene_info.triangles));
  ImGui::Text("Shadow draws %u (%u culled)", scene_info.shadow_draw_calls,
              scene_info.shadow_culled_draws);
  TextView(scene_info.scene_name);
  if (!scene_info.status.empty()) {
    TextView(scene_info.status);
  }
  if (!scene_info.shader_error.empty()) {
    // The full message is in the log; one line keeps the overlay compact.
    const std::string_view first_line = scene_info.shader_error.substr(
        0, scene_info.shader_error.find_first_of("\r\n"));
    ImGui::PushStyleColor(ImGuiCol_Text, kShaderErrorColor);
    TextView(first_line);
    ImGui::PopStyleColor();
  }
  if (!scene_info.help.empty()) {
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
    ImGui::TextDisabled("%.*s", static_cast<int>(scene_info.help.size()),
                        scene_info.help.data());
    ImGui::PopTextWrapPos();
  }
}

}  // namespace

void FrameTimeHistory::Push(float milliseconds) {
  if (count_ == kCapacity) {
    sum_ -= samples_[next_];
  } else {
    ++count_;
  }
  samples_[next_] = milliseconds;
  sum_ += milliseconds;
  next_ = (next_ + 1) % kCapacity;
}

float FrameTimeHistory::min() const {
  return count_ ? *std::min_element(
                      samples_.begin(),
                      samples_.begin() + static_cast<ptrdiff_t>(count_))
                : 0.0f;
}

float FrameTimeHistory::max() const {
  return count_ ? *std::max_element(
                      samples_.begin(),
                      samples_.begin() + static_cast<ptrdiff_t>(count_))
                : 0.0f;
}

PerfOverlay::PerfOverlay(const platform::Window& window,
                         const render::Renderer& renderer)
    : renderer_(renderer),
      layer_(window, renderer, {.scale_mode = UiScaleMode::kWindowHeight}) {}

BuildResult PerfOverlay::Build(float cpu_frame_ms,
                               std::optional<double> gpu_frame_ms,
                               const SceneOverlayInfo& scene_info,
                               input::InputSettings& settings,
                               render::PostSettings& post) {
  cpu_history_.Push(cpu_frame_ms);
  if (gpu_frame_ms) {
    gpu_frame_ms_ = gpu_frame_ms;
  }

  layer_.BeginFrame();

  BuildResult result;
  if (scene_info.show_crosshair) {
    DrawCrosshair();
  }
  if (scene_info.interaction_dot_opacity > 0.0f) {
    DrawInteractionDot(scene_info.interaction_dot_opacity);
  }

  if (visible_ && !scene_info.hide_windows) {
    const ImGuiStyle& style = ImGui::GetStyle();
    ImGui::SetNextWindowPos({style.WindowPadding.x, style.WindowPadding.y},
                            ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.6f);
    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;

    if (ImGui::Begin("Performance", nullptr, kFlags)) {
      DrawTimings(cpu_frame_ms);
      ImGui::Separator();
      DrawDeviceInfo();
      ImGui::Separator();
      DrawCameraInfo(scene_info.camera);
      ImGui::Separator();
      DrawWeaponInfo(scene_info.weapon);
      ImGui::Separator();
      DrawPhysicsInfo(scene_info.physics);
      ImGui::Separator();
      DrawShadowInfo(scene_info.shadows);
      ImGui::Separator();
      DrawSceneInfo(scene_info);
    }
    ImGui::End();

    result.settings_changed = DrawSettings(settings, post);
    if (scene_info.hit_log.log != nullptr) {
      result.clear_hit_log = DrawHitLog(scene_info.hit_log);
    }
  }

  return result;
}

bool PerfOverlay::DrawSettings(input::InputSettings& settings,
                               render::PostSettings& post) const {
  const ImGuiStyle& style = ImGui::GetStyle();
  const ImVec2 display = ImGui::GetIO().DisplaySize;
  ImGui::SetNextWindowPos(
      {display.x - style.WindowPadding.x, style.WindowPadding.y},
      ImGuiCond_Always, {1.0f, 0.0f});
  ImGui::SetNextWindowBgAlpha(0.75f);
  constexpr ImGuiWindowFlags kFlags =
      ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
      ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoCollapse |
      ImGuiWindowFlags_NoMove;

  bool changed = false;
  if (ImGui::Begin("Settings", nullptr, kFlags)) {
    ImGui::PushItemWidth(ImGui::GetFontSize() * 10.0f);
    // Sliders report once the drag ends, so a drag costs one settings save.
    ImGui::SliderFloat("Field of view", &settings.fov_degrees,
                       input::kMinFovDegrees, input::kMaxFovDegrees, "%.0f");
    changed = ImGui::IsItemDeactivatedAfterEdit() || changed;
    ImGui::SliderFloat("Sprint FOV boost", &settings.sprint_fov_boost_degrees,
                       0.0f, input::kMaxSprintFovBoostDegrees, "%.0f");
    changed = ImGui::IsItemDeactivatedAfterEdit() || changed;
    ImGui::SliderFloat("Head bob", &settings.head_bob_intensity, 0.0f, 1.0f,
                       "%.2f");
    changed = ImGui::IsItemDeactivatedAfterEdit() || changed;
    ImGui::SliderFloat("Mouse sensitivity", &settings.mouse_sensitivity,
                       input::kMinMouseSensitivity, input::kMaxMouseSensitivity,
                       "%.4f", ImGuiSliderFlags_Logarithmic);
    changed = ImGui::IsItemDeactivatedAfterEdit() || changed;
    ImGui::PopItemWidth();
    changed = ImGui::Checkbox("Invert Y", &settings.invert_y) || changed;
    changed =
        ImGui::Checkbox("Sprint toggles", &settings.sprint_toggle) || changed;
    changed =
        ImGui::Checkbox("Crouch toggles", &settings.crouch_toggle) || changed;
    DrawPostSettings(post);
    ImGui::TextDisabled("Esc frees the cursor to use this window");
  }
  ImGui::End();
  return changed;
}

bool PerfOverlay::DrawHitLog(const HitLogOverlayInfo& info) const {
  const weapon::TargetLog& log = *info.log;
  const ImGuiStyle& style = ImGui::GetStyle();
  const ImVec2 display = ImGui::GetIO().DisplaySize;
  ImGui::SetNextWindowPos(
      {style.WindowPadding.x, display.y - style.WindowPadding.y},
      ImGuiCond_Always, {0.0f, 1.0f});
  ImGui::SetNextWindowBgAlpha(0.6f);
  constexpr ImGuiWindowFlags kFlags =
      ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
      ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoCollapse |
      ImGuiWindowFlags_NoMove;

  bool clear = false;
  if (ImGui::Begin("Hit log", nullptr, kFlags)) {
    ImGui::Text("Shots %d  hits %d", info.shots_fired, log.total_hits());
    ImGui::SameLine();
    clear = ImGui::SmallButton("Clear");

    // Hits per target, four to a line.
    for (int i = 0; i < log.target_count(); ++i) {
      const size_t index = static_cast<size_t>(i);
      if (index >= info.target_names.size()) {
        break;
      }
      if (i % 4 != 0) {
        ImGui::SameLine();
      }
      const std::string& name = info.target_names[index];
      ImGui::Text("%-14s %3d", name.c_str(), log.hits_on(i));
    }
    ImGui::Separator();

    // Newest first, in a scrolling region wide enough for a whole hit line
    // (plus the scrollbar) at any UI scale.
    const float width = ImGui::CalcTextSize(kWidestHitLine).x +
                        style.ScrollbarSize + 2.0f * style.WindowPadding.x;
    const float height =
        ImGui::GetTextLineHeightWithSpacing() * kHitLogVisibleLines;
    if (ImGui::BeginChild("hits", {width, height})) {
      for (int i = 0; i < log.size(); ++i) {
        DrawHit(log.hit(i), info.target_names);
      }
      if (log.size() == 0) {
        ImGui::TextDisabled("No hits yet");
      }
    }
    ImGui::EndChild();
    ImGui::TextDisabled("Offsets: right/up of the plate centre");
  }
  ImGui::End();
  return clear;
}

void PerfOverlay::DrawTimings(float cpu_frame_ms) const {
  const float average = cpu_history_.average();
  ImGui::Text("%.0f FPS", average > 0.0f ? 1000.0f / average : 0.0f);
  ImGui::Separator();
  ImGui::Text("CPU frame  %7.3f ms", cpu_frame_ms);
  ImGui::Text("  avg/min/max  %.2f / %.2f / %.2f", average, cpu_history_.min(),
              cpu_history_.max());
  if (gpu_frame_ms_) {
    ImGui::Text("GPU frame  %7.3f ms", *gpu_frame_ms_);
  } else {
    ImGui::TextUnformatted("GPU frame     n/a");
  }
  // Every timed pass; a pass that did not run last frame shows n/a.
  for (size_t i = 0; i < render::kGpuScopeCount; ++i) {
    const auto scope = static_cast<render::GpuScope>(i);
    if (scope == render::GpuScope::kTotal) {
      continue;
    }
    const std::optional<double> ms = renderer_.gpu_scope_ms(scope);
    if (ms) {
      ImGui::Text("  %-10s %7.3f ms", render::GpuScopeName(scope), *ms);
    } else {
      ImGui::Text("  %-10s     n/a", render::GpuScopeName(scope));
    }
  }

  const float graph_max = std::max(cpu_history_.max() * 1.2f, 1.0f);
  ImGui::PlotLines(
      "##frametimes", cpu_history_.data(),
      static_cast<int>(cpu_history_.size()),
      static_cast<int>(cpu_history_.offset()), nullptr, 0.0f, graph_max,
      ImVec2(ImGui::GetFontSize() * 18.0f, ImGui::GetFontSize() * 3.5f));
}

void PerfOverlay::DrawDeviceInfo() const {
  const core::Extent2D output = renderer_.output_extent();
  const core::Extent2D render = renderer_.render_extent();
  const char* const vsync = renderer_.vsync() ? "vsync" : "no vsync";
  TextView(renderer_.device_name());
  if (render::UsesTemporalScaler(renderer_.active_upscaling())) {
    // What the scene is rendered at, and what the scaler turns it into.
    const core::Extent2D scaled = renderer_.scene_output_extent();
    ImGui::Text("%u x %u -> %u x %u  |  %s", render.width, render.height,
                scaled.width, scaled.height, vsync);
    ImGui::Text("Upscaling: %s",
                render::UpscalingName(renderer_.active_upscaling()));
  } else {
    ImGui::Text("%u x %u  |  %s", output.width, output.height, vsync);
  }
  const render::TextureStats textures = renderer_.texture_stats();
  ImGui::Text("Materials %zu  textures %zu (%.1f MiB)",
              renderer_.material_count(), textures.count,
              static_cast<double>(textures.bytes) / (1024.0 * 1024.0));
}

}  // namespace blue::ui
