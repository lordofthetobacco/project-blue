#include "ui/imgui_layer.h"

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "core/log.h"
#include "platform/window.h"
#include "render/frame_config.h"
#include "render/renderer.h"
#include "render/vk_check.h"

namespace blue::ui {

namespace {

// Descriptor sets for the font atlas and user textures (the editor
// viewport with its retired generations, editor thumbnails).
constexpr uint32_t kImGuiDescriptorPoolSize = 256;

void CheckImGuiVkResult(VkResult result) { render::CheckVk(result); }

}  // namespace

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
      // The UI has its own rendering pass on the swapchain image, with no
      // depth attachment (render::RecordUiPass).
      .depthAttachmentFormat = VK_FORMAT_UNDEFINED,
  };
  init_info.CheckVkResultFn = CheckImGuiVkResult;

  if (!ImGui_ImplVulkan_Init(&init_info)) {
    core::log::Fatal("ImGui Vulkan backend initialization failed");
  }
}

ImGuiLayer::~ImGuiLayer() {
  renderer_.context().WaitIdle();
  // The backend's pool owns the texture sets; Shutdown frees them all.
  ImGui_ImplVulkan_Shutdown();
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext();
}

void ImGuiLayer::ProcessEvent(const SDL_Event& event) {
  ImGui_ImplSDL3_ProcessEvent(&event);
}

void ImGuiLayer::BeginFrame() {
  ++frame_count_;
  FreeRetiredTextures();
  ImGui_ImplVulkan_NewFrame();
  ImGui_ImplSDL3_NewFrame();  // Refreshes io.DisplaySize from the window.
  UpdateScale();
  ImGui::NewFrame();
}

ImDrawData* ImGuiLayer::EndFrame() {
  ImGui::Render();
  return ImGui::GetDrawData();
}

ImTextureID ImGuiLayer::ViewportTexture() {
  const render::ViewportImage& viewport = renderer_.viewport();
  if (viewport.view == VK_NULL_HANDLE) {
    return ImTextureID{};
  }
  if (viewport.view != viewport_view_) {
    if (viewport_set_ != VK_NULL_HANDLE) {
      // Frames up to the current one may have recorded the old set; a frame
      // begun kFramesInFlight frames later has waited for all of them.
      retired_textures_.push_back(
          {.set = viewport_set_,
           .free_at_frame = frame_count_ + render::kFramesInFlight});
    }
    viewport_set_ = ImGui_ImplVulkan_AddTexture(
        viewport.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    viewport_view_ = viewport.view;
  }
  return reinterpret_cast<ImTextureID>(viewport_set_);
}

ImTextureID ImGuiLayer::AddTexture(VkImageView view) {
  const VkDescriptorSet set = ImGui_ImplVulkan_AddTexture(
      view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  return reinterpret_cast<ImTextureID>(set);
}

void ImGuiLayer::RemoveTexture(ImTextureID texture) {
  if (texture == ImTextureID{}) {
    return;
  }
  // As in ViewportTexture(): frames up to the current one may use it.
  retired_textures_.push_back(
      {.set = reinterpret_cast<VkDescriptorSet>(texture),
       .free_at_frame = frame_count_ + render::kFramesInFlight});
}

void ImGuiLayer::FreeRetiredTextures() {
  const auto done = [this](const RetiredTexture& texture) {
    return texture.free_at_frame <= frame_count_;
  };
  for (const RetiredTexture& texture : retired_textures_) {
    if (done(texture)) {
      ImGui_ImplVulkan_RemoveTexture(texture.set);
    }
  }
  std::erase_if(retired_textures_, done);
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
