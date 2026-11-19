#include "render/passes/ui_pass.h"

#include <imgui_impl_vulkan.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/log.h"
#include "render/barriers.h"
#include "render/frame_config.h"
#include "render/vk_check.h"

namespace blue::render {

namespace {

// Descriptor sets for the font atlas and user textures (the editor
// viewport with its retired generations, editor thumbnails).
constexpr uint32_t kImGuiDescriptorPoolSize = 256;

void CheckImGuiVkResult(VkResult result) { CheckVk(result); }

ImTextureID ToTextureId(VkDescriptorSet set) {
  return reinterpret_cast<ImTextureID>(set);
}

VkDescriptorSet AddTexture(VkImageView view) {
  return ImGui_ImplVulkan_AddTexture(view,
                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

}  // namespace

void RecordUiPass(const FrameContext& ctx, ImDrawData* ui,
                  UiBackground background) {
  const bool clear = background == UiBackground::kClear;
  if (clear) {
    // Nothing to wait for but the acquire semaphore, which the submission
    // waits for at the color attachment output stage.
    ImageBarrier(
        ctx.cmd, ctx.swapchain_image, VK_IMAGE_ASPECT_COLOR_BIT,
        {VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_NONE},
        kColorAttachmentWrite, VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  } else if (ui != nullptr) {
    // Rendering scopes do not order attachment accesses between them.
    ImageBarrier(ctx.cmd, ctx.swapchain_image, VK_IMAGE_ASPECT_COLOR_BIT,
                 kColorAttachmentWrite, kColorAttachmentReadWrite,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  }
  if (ui != nullptr || clear) {
    const VkRenderingAttachmentInfo color_attachment{
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = ctx.swapchain_view,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp =
            clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = {.color = {{0.0f, 0.0f, 0.0f, 1.0f}}},
    };
    const VkRenderingInfo rendering_info{
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = {{0, 0}, ctx.swapchain_extent},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color_attachment,
    };
    vkCmdBeginRendering(ctx.cmd, &rendering_info);
    if (ui != nullptr) {
      // Only checks that the backend is initialized; ImGuiLayer used to call
      // it before ImGui::NewFrame().
      ImGui_ImplVulkan_NewFrame();
      ImGui_ImplVulkan_RenderDrawData(ui, ctx.cmd);
    }
    vkCmdEndRendering(ctx.cmd);
  }
  ImageBarrier(ctx.cmd, ctx.swapchain_image, VK_IMAGE_ASPECT_COLOR_BIT,
               kColorAttachmentWrite, kNoAccess,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
}

void UiRenderer::Init(const VulkanContext& context,
                      const Swapchain& swapchain) {
  const VkFormat color_format = swapchain.format();

  ImGui_ImplVulkan_InitInfo init_info{};
  init_info.ApiVersion = VulkanContext::kApiVersion;
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
      // depth attachment (RecordUiPass()).
      .depthAttachmentFormat = VK_FORMAT_UNDEFINED,
  };
  init_info.CheckVkResultFn = CheckImGuiVkResult;

  if (!ImGui_ImplVulkan_Init(&init_info)) {
    core::log::Fatal("ImGui Vulkan backend initialization failed");
  }
  initialized_ = true;
}

void UiRenderer::Shutdown() {
  if (!initialized_) {
    return;
  }
  // The backend's pool owns the texture sets; Shutdown frees them all.
  ImGui_ImplVulkan_Shutdown();
  initialized_ = false;
  viewport_set_ = VK_NULL_HANDLE;
  default_set_ = VK_NULL_HANDLE;
  texture_sets_.clear();
  retired_.clear();
}

ImTextureID UiRenderer::SetViewport(VkImageView view, uint64_t frame_serial) {
  if (!initialized_) {
    return ImTextureID{};
  }
  if (viewport_set_ != VK_NULL_HANDLE) {
    retired_.push_back({.set = viewport_set_, .last_frame = frame_serial});
  }
  viewport_set_ = AddTexture(view);
  return ToTextureId(viewport_set_);
}

ImTextureID UiRenderer::Texture(TextureId id, const TextureCache& textures) {
  if (!initialized_) {
    return ImTextureID{};
  }
  const VkImageView view = textures.view(id);
  if (view == textures.default_view(DefaultTexture::kWhite)) {
    if (default_set_ == VK_NULL_HANDLE) {
      default_set_ = AddTexture(view);
    }
    return ToTextureId(default_set_);
  }
  const auto index = static_cast<size_t>(id);
  if (index >= texture_sets_.size()) {
    texture_sets_.resize(index + 1, VK_NULL_HANDLE);
  }
  if (texture_sets_[index] == VK_NULL_HANDLE) {
    texture_sets_[index] = AddTexture(view);
  }
  return ToTextureId(texture_sets_[index]);
}

void UiRenderer::Release(TextureId id, uint64_t frame_serial) {
  const auto index = static_cast<size_t>(id);
  if (index >= texture_sets_.size() ||
      texture_sets_[index] == VK_NULL_HANDLE) {
    return;
  }
  retired_.push_back({.set = texture_sets_[index], .last_frame = frame_serial});
  texture_sets_[index] = VK_NULL_HANDLE;
}

void UiRenderer::FreeRetired(uint64_t frame_serial) {
  const auto done = [frame_serial](const RetiredSet& retired) {
    return retired.last_frame + kFramesInFlight <= frame_serial;
  };
  for (const RetiredSet& retired : retired_) {
    if (done(retired)) {
      ImGui_ImplVulkan_RemoveTexture(retired.set);
    }
  }
  std::erase_if(retired_, done);
}

}  // namespace blue::render
