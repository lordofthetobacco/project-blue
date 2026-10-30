#include "ui/perf_overlay.h"

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>
#include <vulkan/vk_enum_string_helper.h>

#include <algorithm>

#include "core/log.h"
#include "lighting/cascades.h"
#include "platform/window.h"
#include "render/renderer.h"
#include "render/vk_check.h"

namespace blue::ui {

namespace {

constexpr uint32_t kImGuiDescriptorPoolSize = 8;
constexpr ImVec4 kShaderErrorColor{1.0f, 0.3f, 0.3f, 1.0f};

void CheckImGuiVkResult(VkResult result) { render::CheckVk(result); }

const char* PresentModeLabel(VkPresentModeKHR mode) {
  switch (mode) {
    case VK_PRESENT_MODE_FIFO_KHR:
      return "FIFO (VSync on)";
    case VK_PRESENT_MODE_MAILBOX_KHR:
      return "Mailbox (VSync off)";
    case VK_PRESENT_MODE_IMMEDIATE_KHR:
      return "Immediate (VSync off)";
    default:
      return string_VkPresentModeKHR(mode);
  }
}

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
  if (shadows.shadow_ms) {
    ImGui::Text("  shadow pass %7.3f ms", *shadows.shadow_ms);
  } else {
    ImGui::TextUnformatted("  shadow pass     n/a");
  }
  if (shadows.main_ms) {
    ImGui::Text("  main pass   %7.3f ms", *shadows.main_ms);
  } else {
    ImGui::TextUnformatted("  main pass       n/a");
  }
  ImGui::Text("  cascade debug: %s", shadows.cascade_debug ? "on" : "off");
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

void DrawSceneInfo(const SceneOverlayInfo& scene_info) {
  ImGui::Text("Entities %zu  |  %u draws  |  %llu tris",
              scene_info.entity_count, scene_info.draw_calls,
              static_cast<unsigned long long>(scene_info.triangles));
  TextView(scene_info.scene_path);
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
    : renderer_(renderer) {
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();

  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

  const float scale = window.DisplayScale();
  ImGuiStyle& style = ImGui::GetStyle();
  ImGui::StyleColorsDark();
  style.ScaleAllSizes(scale);
  style.FontScaleDpi = scale;
  style.WindowRounding = 4.0f * scale;

  ImGui_ImplSDL3_InitForVulkan(window.handle());

  const render::VulkanContext& context = renderer.context();
  const render::Swapchain& swapchain = renderer.swapchain();
  const VkFormat color_format = swapchain.format();

  ImGui_ImplVulkan_InitInfo init_info{};
  init_info.ApiVersion = render::VulkanContext::kApiVersion;
  init_info.Instance = context.instance();
  init_info.PhysicalDevice = context.physical_device();
  init_info.Device = context.device();
  init_info.QueueFamily = context.queue_family();
  init_info.Queue = context.queue();
  init_info.DescriptorPoolSize = kImGuiDescriptorPoolSize;
  init_info.MinImageCount = swapchain.min_image_count();
  init_info.ImageCount = swapchain.image_count();
  init_info.UseDynamicRendering = true;
  init_info.PipelineInfoMain.PipelineRenderingCreateInfo = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
      .colorAttachmentCount = 1,
      .pColorAttachmentFormats = &color_format,
      // The overlay is drawn inside the scene's rendering pass, which has a
      // depth attachment.
      .depthAttachmentFormat = render::Renderer::kDepthFormat,
  };
  init_info.CheckVkResultFn = CheckImGuiVkResult;

  if (!ImGui_ImplVulkan_Init(&init_info)) {
    core::log::Fatal("ImGui Vulkan backend initialization failed");
  }
}

PerfOverlay::~PerfOverlay() {
  renderer_.context().WaitIdle();
  ImGui_ImplVulkan_Shutdown();
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext();
}

void PerfOverlay::ProcessEvent(const SDL_Event& event) {
  ImGui_ImplSDL3_ProcessEvent(&event);
}

bool PerfOverlay::WantsMouse() const {
  return ImGui::GetCurrentContext() != nullptr &&
         ImGui::GetIO().WantCaptureMouse;
}

bool PerfOverlay::WantsKeyboard() const {
  return ImGui::GetCurrentContext() != nullptr &&
         ImGui::GetIO().WantCaptureKeyboard;
}

BuildResult PerfOverlay::Build(float cpu_frame_ms,
                               std::optional<double> gpu_frame_ms,
                               const SceneOverlayInfo& scene_info,
                               input::InputSettings& settings) {
  cpu_history_.Push(cpu_frame_ms);
  if (gpu_frame_ms) {
    gpu_frame_ms_ = gpu_frame_ms;
  }

  ImGui_ImplVulkan_NewFrame();
  ImGui_ImplSDL3_NewFrame();
  ImGui::NewFrame();

  BuildResult result;
  if (scene_info.show_crosshair) {
    DrawCrosshair();
  }

  if (visible_) {
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
      DrawPhysicsInfo(scene_info.physics);
      ImGui::Separator();
      DrawShadowInfo(scene_info.shadows);
      ImGui::Separator();
      DrawSceneInfo(scene_info);
    }
    ImGui::End();

    result.settings_changed = DrawSettings(settings);
  }

  ImGui::Render();
  result.draw_data = ImGui::GetDrawData();
  return result;
}

bool PerfOverlay::DrawSettings(input::InputSettings& settings) const {
  const ImGuiStyle& style = ImGui::GetStyle();
  const ImVec2 display = ImGui::GetIO().DisplaySize;
  ImGui::SetNextWindowPos({display.x - style.WindowPadding.x,
                           style.WindowPadding.y},
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
                       input::kMinMouseSensitivity,
                       input::kMaxMouseSensitivity, "%.4f",
                       ImGuiSliderFlags_Logarithmic);
    changed = ImGui::IsItemDeactivatedAfterEdit() || changed;
    ImGui::PopItemWidth();
    changed = ImGui::Checkbox("Invert Y", &settings.invert_y) || changed;
    changed = ImGui::Checkbox("Sprint toggles", &settings.sprint_toggle) ||
              changed;
    changed = ImGui::Checkbox("Crouch toggles", &settings.crouch_toggle) ||
              changed;
    ImGui::TextDisabled("Esc frees the cursor to use this window");
  }
  ImGui::End();
  return changed;
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

  const float graph_max = std::max(cpu_history_.max() * 1.2f, 1.0f);
  ImGui::PlotLines(
      "##frametimes", cpu_history_.data(),
      static_cast<int>(cpu_history_.size()),
      static_cast<int>(cpu_history_.offset()), nullptr, 0.0f, graph_max,
      ImVec2(ImGui::GetFontSize() * 18.0f, ImGui::GetFontSize() * 3.5f));
}

void PerfOverlay::DrawDeviceInfo() const {
  const render::Swapchain& swapchain = renderer_.swapchain();
  TextView(renderer_.context().device_name());
  ImGui::Text("%u x %u  |  %s", swapchain.extent().width,
              swapchain.extent().height,
              PresentModeLabel(swapchain.present_mode()));
}

}  // namespace blue::ui
