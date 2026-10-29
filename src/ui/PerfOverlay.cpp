#include "ui/PerfOverlay.hpp"

#include "platform/Window.hpp"
#include "render/Renderer.hpp"
#include "render/VkCheck.hpp"

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>

#include <algorithm>

namespace blue {

namespace {

constexpr uint32_t kImGuiDescriptorPoolSize = 8;

void checkImGuiVkResult(VkResult result)
{
    VK_CHECK(result);
}

const char* presentModeLabel(VkPresentModeKHR mode)
{
    switch (mode) {
    case VK_PRESENT_MODE_FIFO_KHR: return "FIFO (VSync on)";
    case VK_PRESENT_MODE_MAILBOX_KHR: return "Mailbox (VSync off)";
    case VK_PRESENT_MODE_IMMEDIATE_KHR: return "Immediate (VSync off)";
    default: return string_VkPresentModeKHR(mode);
    }
}

} // namespace

void FrameTimeHistory::push(float milliseconds)
{
    if (count_ == kCapacity) {
        sum_ -= samples_[next_];
    } else {
        ++count_;
    }
    samples_[next_] = milliseconds;
    sum_ += milliseconds;
    next_ = (next_ + 1) % kCapacity;
}

float FrameTimeHistory::min() const
{
    return count_ ? *std::min_element(samples_.begin(), samples_.begin() + static_cast<ptrdiff_t>(count_)) : 0.0f;
}

float FrameTimeHistory::max() const
{
    return count_ ? *std::max_element(samples_.begin(), samples_.begin() + static_cast<ptrdiff_t>(count_)) : 0.0f;
}

PerfOverlay::PerfOverlay(const Window& window, const Renderer& renderer)
    : renderer_(renderer)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

    const float scale = window.displayScale();
    ImGuiStyle& style = ImGui::GetStyle();
    ImGui::StyleColorsDark();
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
    style.WindowRounding = 4.0f * scale;

    ImGui_ImplSDL3_InitForVulkan(window.handle());

    const VulkanContext& context = renderer.context();
    const Swapchain& swapchain = renderer.swapchain();
    const VkFormat colorFormat = swapchain.format();

    ImGui_ImplVulkan_InitInfo initInfo{};
    initInfo.ApiVersion = VulkanContext::kApiVersion;
    initInfo.Instance = context.instance();
    initInfo.PhysicalDevice = context.physicalDevice();
    initInfo.Device = context.device();
    initInfo.QueueFamily = context.queueFamily();
    initInfo.Queue = context.queue();
    initInfo.DescriptorPoolSize = kImGuiDescriptorPoolSize;
    initInfo.MinImageCount = swapchain.minImageCount();
    initInfo.ImageCount = swapchain.imageCount();
    initInfo.UseDynamicRendering = true;
    initInfo.PipelineInfoMain.PipelineRenderingCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &colorFormat,
        // The overlay is drawn inside the scene's rendering pass, which has a depth attachment.
        .depthAttachmentFormat = Renderer::kDepthFormat,
    };
    initInfo.CheckVkResultFn = checkImGuiVkResult;

    if (!ImGui_ImplVulkan_Init(&initInfo)) {
        log::fatal("ImGui Vulkan backend initialization failed");
    }
}

PerfOverlay::~PerfOverlay()
{
    renderer_.context().waitIdle();
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
}

void PerfOverlay::processEvent(const SDL_Event& event)
{
    ImGui_ImplSDL3_ProcessEvent(&event);
}

ImDrawData* PerfOverlay::build(float cpuFrameMs, std::optional<double> gpuFrameMs, const SceneOverlayInfo& scene)
{
    cpuHistory_.push(cpuFrameMs);
    if (gpuFrameMs) {
        gpuFrameMs_ = gpuFrameMs;
    }

    if (!visible_) {
        return nullptr;
    }

    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    const ImGuiStyle& style = ImGui::GetStyle();
    ImGui::SetNextWindowPos({style.WindowPadding.x, style.WindowPadding.y}, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.6f);
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                       ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;

    if (ImGui::Begin("Performance", nullptr, flags)) {
        const float average = cpuHistory_.average();
        ImGui::Text("%.0f FPS", average > 0.0f ? 1000.0f / average : 0.0f);
        ImGui::Separator();
        ImGui::Text("CPU frame  %7.3f ms", cpuFrameMs);
        ImGui::Text("  avg/min/max  %.2f / %.2f / %.2f", average, cpuHistory_.min(), cpuHistory_.max());
        if (gpuFrameMs_) {
            ImGui::Text("GPU frame  %7.3f ms", *gpuFrameMs_);
        } else {
            ImGui::TextUnformatted("GPU frame     n/a");
        }

        const float graphMax = std::max(cpuHistory_.max() * 1.2f, 1.0f);
        ImGui::PlotLines("##frametimes", cpuHistory_.data(), static_cast<int>(cpuHistory_.size()),
                         static_cast<int>(cpuHistory_.offset()), nullptr, 0.0f, graphMax,
                         ImVec2(ImGui::GetFontSize() * 18.0f, ImGui::GetFontSize() * 3.5f));

        ImGui::Separator();
        const Swapchain& swapchain = renderer_.swapchain();
        const std::string_view gpuName = renderer_.context().deviceName();
        ImGui::Text("%.*s", static_cast<int>(gpuName.size()), gpuName.data());
        ImGui::Text("%u x %u  |  %s", swapchain.extent().width, swapchain.extent().height,
                    presentModeLabel(swapchain.presentMode()));
        ImGui::Separator();
        const CameraOverlayInfo& camera = scene.camera;
        ImGui::Text("Camera: %.*s", static_cast<int>(camera.mode.size()), camera.mode.data());
        ImGui::Text("  pos %.1f %.1f %.1f", camera.position.x, camera.position.y, camera.position.z);
        ImGui::Text("  yaw %.1f  pitch %.1f deg", camera.yawDegrees, camera.pitchDegrees);
        ImGui::Text("  speed %.1f m/s  sens %.4f rad/count", camera.flySpeed, camera.sensitivity);
        ImGui::Text("  Invert Y: %s", camera.invertY ? "on" : "off");
        ImGui::TextDisabled("%.*s", static_cast<int>(camera.hint.size()), camera.hint.data());

        ImGui::Separator();
        ImGui::Text("Entities %zu  |  %u draws  |  %llu tris", scene.entityCount, scene.drawCalls,
                    static_cast<unsigned long long>(scene.triangles));
        ImGui::Text("%.*s", static_cast<int>(scene.scenePath.size()), scene.scenePath.data());
        if (!scene.status.empty()) {
            ImGui::Text("%.*s", static_cast<int>(scene.status.size()), scene.status.data());
        }
        ImGui::TextDisabled("WASD move  Tab fly/orbit  I invert Y  [ ] sens  F1 overlay  F5 save  F9 load  V vsync  Esc quit");
    }
    ImGui::End();

    ImGui::Render();
    return ImGui::GetDrawData();
}

} // namespace blue
