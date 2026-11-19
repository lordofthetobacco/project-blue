#include "render/renderer.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <glm/vec4.hpp>

#include "camera/frustum.h"
#include "core/log.h"
#include "lighting/cascades.h"
#include "material/material.h"
#include "platform/window.h"
#include "render/barriers.h"
#include "render/frame_uniforms.h"
#include "render/passes/scene_pass.h"
#include "render/passes/ui_pass.h"
#include "render/renderer_impl.h"
#include "render/vk_check.h"
#include "render/vk_extent.h"

namespace blue::render {

namespace {

// Neutral grey dielectric: what kDefaultMaterial looks like.
material::MaterialDesc DefaultMaterialDesc() {
  material::MaterialDesc desc;
  desc.name = "default";
  desc.base_color = glm::vec4(0.5f, 0.5f, 0.5f, 1.0f);
  desc.roughness = 0.6f;
  return desc;
}

// Returns the format of the editor viewport image: like the swapchain, sRGB
// or not, so the UI shows it with the same encoding as the game window.
VkFormat ViewportFormat(VkFormat swapchain_format) {
  return swapchain_format == VK_FORMAT_B8G8R8A8_SRGB ||
                 swapchain_format == VK_FORMAT_R8G8B8A8_SRGB
             ? VK_FORMAT_R8G8B8A8_SRGB
             : VK_FORMAT_R8G8B8A8_UNORM;
}

}  // namespace

RendererImpl::RendererImpl(const platform::Window& window,
                           const std::string& shader_dir,
                           shader::CompileMode shader_mode)
    : window_(window),
      shader_dir_(shader_dir),
      context_(window),
      swapchain_(context_, ToVkExtent(window.PixelSize()), vsync_),
      samplers_(context_),
      targets_(context_, swapchain_.extent()),
      frame_globals_(context_, samplers_),
      textures_(context_),
      materials_(context_, textures_, samplers_),
      shader_programs_(context_.device(), shader_mode),
      environment_(context_, shader_programs_, shader_dir, samplers_,
                   frame_globals_),
      shadow_pass_(context_, shader_programs_, shader_dir, meshes_),
      depth_prepass_(context_, shader_programs_, shader_dir,
                     frame_globals_.set_layout(), materials_, meshes_),
      ssao_pass_(context_, shader_programs_, shader_dir),
      mesh_pass_(context_, shader_programs_, shader_dir,
                 frame_globals_.set_layout(), materials_, meshes_),
      skybox_pass_(context_, shader_programs_, shader_dir,
                   frame_globals_.set_layout(), environment_),
      reticle_pass_(context_, shader_programs_, shader_dir,
                    frame_globals_.set_layout()),
      bloom_pass_(context_, shader_programs_, shader_dir),
      tonemap_pass_(context_, shader_programs_, shader_dir),
      overlay_pass_(context_, shader_programs_, shader_dir,
                    frame_globals_.set_layout(), meshes_),
      present_pass_(context_, shader_programs_, shader_dir,
                    swapchain_.format()),
      gpu_timer_(context_) {
  const MaterialId default_material = materials_.Create(DefaultMaterialDesc());
  if (default_material != kDefaultMaterial) {
    core::log::Fatal("The default material must be created first");
  }
  frame_globals_.BindShadowMap(shadow_pass_.map_view(), shadow_pass_.sampler());
  BindTargets();
  CreateFrames();
}

RendererImpl::~RendererImpl() {
  context_.WaitIdle();

  const VkDevice device = context_.device();
  for (Frame& frame : frames_) {
    vkDestroySemaphore(device, frame.image_acquired, nullptr);
    vkDestroyFence(device, frame.in_flight, nullptr);
    vkDestroyCommandPool(device, frame.command_pool, nullptr);
  }
}

void RendererImpl::CreateFrames() {
  const VkDevice device = context_.device();

  for (Frame& frame : frames_) {
    const VkCommandPoolCreateInfo pool_info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
        .queueFamilyIndex = context_.queue_family(),
    };
    CheckVk(
        vkCreateCommandPool(device, &pool_info, nullptr, &frame.command_pool));

    const VkCommandBufferAllocateInfo alloc_info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = frame.command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    CheckVk(
        vkAllocateCommandBuffers(device, &alloc_info, &frame.command_buffer));

    const VkFenceCreateInfo fence_info{
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };
    CheckVk(vkCreateFence(device, &fence_info, nullptr, &frame.in_flight));

    const VkSemaphoreCreateInfo semaphore_info{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    CheckVk(vkCreateSemaphore(device, &semaphore_info, nullptr,
                              &frame.image_acquired));
  }
}

void RendererImpl::BindTargets() {
  frame_globals_.BindAmbientOcclusion(targets_.ssao().view());
  ssao_pass_.BindTargets(targets_, samplers_);
  bloom_pass_.BindTargets(targets_, samplers_);
  tonemap_pass_.BindTargets(targets_, samplers_);
  overlay_pass_.BindTargets(targets_, samplers_);
  present_pass_.BindTargets(targets_, samplers_);
  if (viewport_present_pass_) {
    viewport_present_pass_->BindTargets(targets_, samplers_);
  }
}

MeshId RendererImpl::UploadMesh(const asset::MeshData& data) {
  meshes_.emplace_back(context_, data);
  return static_cast<MeshId>(meshes_.size() - 1);
}

void RendererImpl::ReleaseMesh(MeshId id) {
  const auto index = static_cast<size_t>(id);
  if (index >= meshes_.size() || !meshes_[index].resident()) {
    return;
  }
  // The moved-from mesh left in the slot is the tombstone (resident() false).
  retired_meshes_.push_back(
      {.object = std::move(meshes_[index]), .last_frame = frame_serial_});
}

void RendererImpl::ReleaseTextures(std::span<const TextureId> ids) {
  for (const TextureId id : ids) {
    ui_.Release(id, frame_serial_);
    Image image = textures_.TakeUploaded(id);
    if (image.handle() != VK_NULL_HANDLE) {
      retired_images_.push_back(
          {.object = std::move(image), .last_frame = frame_serial_});
    }
  }
}

void RendererImpl::DestroyRetired() {
  const auto done = [this](uint64_t last_frame) {
    return last_frame + kFramesInFlight <= frame_serial_;
  };
  std::erase_if(retired_meshes_, [&](const Retired<Mesh>& retired) {
    return done(retired.last_frame);
  });
  std::erase_if(retired_images_, [&](const Retired<Image>& retired) {
    return done(retired.last_frame);
  });
  ui_.FreeRetired(frame_serial_);
}

void RendererImpl::SetVsync(bool enabled) {
  if (vsync_ != enabled) {
    vsync_ = enabled;
    swapchain_dirty_ = true;
  }
}

bool RendererImpl::RebuildSwapchain() {
  context_.WaitIdle();
  if (!swapchain_.Recreate(ToVkExtent(window_.PixelSize()), vsync_)) {
    return false;  // Surface has no area; stay dirty and retry next frame.
  }
  if (viewport_.generation == 0) {  // Otherwise they follow the viewport.
    targets_.Resize(swapchain_.extent());
    BindTargets();
  }
  swapchain_dirty_ = false;
  return true;
}

void RendererImpl::WaitForFramesInFlight() const {
  std::array<VkFence, kFramesInFlight> fences{};
  for (size_t i = 0; i < kFramesInFlight; ++i) {
    fences[i] = frames_[i].in_flight;
  }
  CheckVk(vkWaitForFences(context_.device(), kFramesInFlight, fences.data(),
                          VK_TRUE, UINT64_MAX));
}

ViewportImage RendererImpl::PrepareViewport(VkExtent2D size) {
  size = {std::max(size.width, 1u), std::max(size.height, 1u)};
  if (!viewport_present_pass_) {
    viewport_present_pass_.emplace(context_, shader_programs_, shader_dir_,
                                   ViewportFormat(swapchain_.format()));
    viewport_present_pass_->BindTargets(targets_, samplers_);
  }
  const bool changed = size.width != viewport_.extent.width ||
                       size.height != viewport_.extent.height;
  const bool may_resize =
      viewport_.generation == 0 ||
      frame_serial_ >= viewport_resized_at_ + Renderer::kViewportResizeFrames;
  if (changed && may_resize) {
    ResizeViewport(size);
  }
  return viewport_;
}

void RendererImpl::ResizeViewport(VkExtent2D size) {
  WaitForFramesInFlight();
  viewport_image_ =
      Image(context_, {.extent = size,
                       .format = ViewportFormat(swapchain_.format()),
                       .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                VK_IMAGE_USAGE_SAMPLED_BIT,
                       .dedicated = true});
  // The UI may sample it before the first DrawViewportFrame() (a frame that
  // skipped drawing): give it a defined layout.
  context_.SubmitImmediate([this](VkCommandBuffer cmd) {
    ImageBarrier(cmd, viewport_image_.handle(), VK_IMAGE_ASPECT_COLOR_BIT,
                 kNoAccess, kFragmentSampledRead, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  });
  targets_.Resize(size);
  BindTargets();
  // Frames up to the current one may have recorded the old texture.
  viewport_ = {
      .texture = ui_.SetViewport(viewport_image_.view(), frame_serial_),
      .extent = ToExtent(size),
      .generation = viewport_.generation + 1,
  };
  viewport_resized_at_ = frame_serial_;
  core::log::Info("Viewport output {} x {}", size.width, size.height);
}

uint64_t RendererImpl::RequestViewportReadback() {
  const uint64_t ticket = next_readback_ticket_++;
  readbacks_.push_back({.ticket = ticket});
  return ticket;
}

ReadbackStatus RendererImpl::TakeViewportReadback(uint64_t ticket,
                                              ViewportPixels& pixels) {
  const auto it = std::ranges::find(readbacks_, ticket, &Readback::ticket);
  if (it == readbacks_.end()) {
    return ReadbackStatus::kUnknown;
  }
  // The fence of a frame is waited on kFramesInFlight frames later (as for
  // DestroyRetired()).
  if (it->frame_serial == 0 ||
      it->frame_serial + kFramesInFlight > frame_serial_) {
    return ReadbackStatus::kPending;
  }
  it->buffer.InvalidateForRead();
  const auto* source = static_cast<const std::byte*>(it->buffer.mapped());
  pixels.width = it->extent.width;
  pixels.height = it->extent.height;
  pixels.rgba.assign(source, source + it->buffer.size());
  readbacks_.erase(it);
  return ReadbackStatus::kReady;
}

void RendererImpl::RecordViewportReadbacks(VkCommandBuffer cmd) {
  constexpr BarrierScope kTransferRead{VK_PIPELINE_STAGE_2_COPY_BIT,
                                       VK_ACCESS_2_TRANSFER_READ_BIT};
  bool transitioned = false;
  for (Readback& readback : readbacks_) {
    if (readback.frame_serial != 0) {
      continue;
    }
    if (!transitioned) {
      ImageBarrier(cmd, viewport_image_.handle(), VK_IMAGE_ASPECT_COLOR_BIT,
                   kColorAttachmentWrite, kTransferRead,
                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
      transitioned = true;
    }
    readback.extent = ToVkExtent(viewport_.extent);
    readback.frame_serial = frame_serial_;
    readback.buffer =
        Buffer(context_.allocator(),
               VkDeviceSize{readback.extent.width} * readback.extent.height * 4,
               VK_BUFFER_USAGE_TRANSFER_DST_BIT, Buffer::Access::kCpuRead);
    const VkBufferImageCopy region{
        .bufferOffset = 0,
        .bufferRowLength = 0,  // Tightly packed.
        .bufferImageHeight = 0,
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageOffset = {0, 0, 0},
        .imageExtent = {readback.extent.width, readback.extent.height, 1},
    };
    vkCmdCopyImageToBuffer(cmd, viewport_image_.handle(),
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           readback.buffer.handle(), 1, &region);
  }
  if (!transitioned) {
    return;
  }
  // The host reads the buffers after the frame's fence.
  const VkMemoryBarrier2 host_barrier{
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
      .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
      .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
      .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT,
  };
  const VkDependencyInfo dependency{
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .memoryBarrierCount = 1,
      .pMemoryBarriers = &host_barrier,
  };
  vkCmdPipelineBarrier2(cmd, &dependency);
  ImageBarrier(cmd, viewport_image_.handle(), VK_IMAGE_ASPECT_COLOR_BIT,
               kTransferRead, kColorAttachmentWrite,
               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
}

void RendererImpl::DrawFrameTo(const SceneView& view, ImDrawData* ui,
                           Output output) {
  const core::Extent2D window_size = window_.PixelSize();
  if (window_size.width == 0 || window_size.height == 0) {
    return;  // Minimized: nothing to present to.
  }
  if (swapchain_dirty_ && !RebuildSwapchain()) {
    return;
  }

  const Frame& frame = frames_[frame_index_];
  CheckVk(vkWaitForFences(context_.device(), 1, &frame.in_flight, VK_TRUE,
                          UINT64_MAX));
  gpu_timer_.Collect(frame_index_);

  const std::optional<uint32_t> image_index = AcquireImage(frame);
  if (!image_index) {
    return;
  }

  // Reset only once we know work will be submitted, otherwise the next wait
  // deadlocks.
  CheckVk(vkResetFences(context_.device(), 1, &frame.in_flight));
  ++frame_serial_;
  materials_.BeginFrame(frame_serial_);
  DestroyRetired();

  const bool shadows = view.shadows_enabled && view.sun.casts_shadows;
  Cascades cascades{};
  const VkExtent2D extent = targets_.extent();
  if (shadows) {
    const float aspect = static_cast<float>(extent.width) /
                         static_cast<float>(std::max(extent.height, 1u));
    cascades = lighting::FitCascades(
        view.camera.view, view.camera.vertical_fov_radians, aspect,
        view.camera.near_plane, view.sun.direction);
  }

  ComputeWorldBounds(view);
  const camera::Frustum camera_frustum(view.camera.projection *
                                       view.camera.view);
  stats_ = {};

  draw_list_.Build(view, world_bounds_, camera_frustum, materials_, meshes_,
                   stats_);
  FrameUniforms uniforms =
      MakeFrameUniforms(view, cascades, shadows, ToExtent(extent));
  environment_.FillUniforms(uniforms);
  frame_globals_.Write(frame_index_, uniforms);
  const FrameContext ctx{
      .cmd = frame.command_buffer,
      .frame_index = frame_index_,
      .frame_set = frame_globals_.set(frame_index_),
      .view = view,
      .targets = targets_,
      .swapchain_image = swapchain_.image(*image_index),
      .swapchain_view = swapchain_.image_view(*image_index),
      .swapchain_extent = swapchain_.extent(),
      .world_bounds = world_bounds_,
      .camera_frustum = camera_frustum,
      .cascades = cascades,
      .shadows = shadows,
      .draw_list = draw_list_.entries(),
      .stats = stats_,
  };
  RecordCommands(frame, ctx, ui, output);
  SubmitAndPresent(frame, *image_index);

  frame_index_ = (frame_index_ + 1) % kFramesInFlight;
}

std::optional<uint32_t> RendererImpl::AcquireImage(const Frame& frame) {
  uint32_t image_index = 0;
  const VkResult result =
      vkAcquireNextImageKHR(context_.device(), swapchain_.handle(), UINT64_MAX,
                            frame.image_acquired, VK_NULL_HANDLE, &image_index);
  if (result == VK_ERROR_OUT_OF_DATE_KHR) {
    swapchain_dirty_ = true;
    return std::nullopt;
  }
  if (result == VK_SUBOPTIMAL_KHR) {
    swapchain_dirty_ = true;  // Still presentable; rebuild next frame.
  } else {
    CheckVk(result);
  }
  return image_index;
}

void RendererImpl::SubmitAndPresent(const Frame& frame, uint32_t image_index) {
  const VkSemaphore render_finished = swapchain_.render_finished(image_index);
  const VkSemaphoreSubmitInfo wait_info{
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
      .semaphore = frame.image_acquired,
      .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
  };
  const VkSemaphoreSubmitInfo signal_info{
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
      .semaphore = render_finished,
      .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
  };
  const VkCommandBufferSubmitInfo cmd_info{
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
      .commandBuffer = frame.command_buffer,
  };
  const VkSubmitInfo2 submit_info{
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
      .waitSemaphoreInfoCount = 1,
      .pWaitSemaphoreInfos = &wait_info,
      .commandBufferInfoCount = 1,
      .pCommandBufferInfos = &cmd_info,
      .signalSemaphoreInfoCount = 1,
      .pSignalSemaphoreInfos = &signal_info,
  };
  CheckVk(vkQueueSubmit2(context_.queue(), 1, &submit_info, frame.in_flight));

  const VkSwapchainKHR swapchain = swapchain_.handle();
  const VkPresentInfoKHR present_info{
      .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
      .waitSemaphoreCount = 1,
      .pWaitSemaphores = &render_finished,
      .swapchainCount = 1,
      .pSwapchains = &swapchain,
      .pImageIndices = &image_index,
  };
  const VkResult present_result =
      vkQueuePresentKHR(context_.queue(), &present_info);
  if (present_result == VK_ERROR_OUT_OF_DATE_KHR ||
      present_result == VK_SUBOPTIMAL_KHR) {
    swapchain_dirty_ = true;
  } else {
    CheckVk(present_result);
  }
}

void RendererImpl::ComputeWorldBounds(const SceneView& view) {
  world_bounds_.clear();
  world_bounds_.reserve(view.draws.size());
  for (const DrawItem& item : view.draws) {
    world_bounds_.push_back(camera::TransformAabb(
        {.min = item.bounds.min, .max = item.bounds.max}, item.transform));
  }
}

void RendererImpl::RecordCommands(const Frame& frame, const FrameContext& ctx,
                              ImDrawData* ui, Output output) {
  const VkCommandBuffer cmd = ctx.cmd;
  CheckVk(vkResetCommandPool(context_.device(), frame.command_pool, 0));
  const VkCommandBufferBeginInfo begin_info{
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
  };
  CheckVk(vkBeginCommandBuffer(cmd, &begin_info));
  gpu_timer_.BeginFrame(cmd, frame_index_);
  gpu_timer_.Begin(cmd, frame_index_, GpuScope::kTotal);

  // Times `record` as `scope`.
  const auto timed = [&](GpuScope scope, const auto& record) {
    gpu_timer_.Begin(cmd, frame_index_, scope);
    record();
    gpu_timer_.End(cmd, frame_index_, scope);
  };

  if (ctx.shadows) {
    timed(GpuScope::kShadow, [&] { shadow_pass_.Record(ctx); });
  } else {
    shadow_pass_.Record(ctx);  // Only initializes the map's layout.
  }
  timed(GpuScope::kDepthPrepass, [&] { depth_prepass_.Record(ctx); });
  if (ctx.view.post.ssao_enabled) {
    timed(GpuScope::kSsao, [&] { ssao_pass_.Record(ctx); });
  } else {
    ssao_pass_.Record(ctx);  // At most a one-time clear to white.
  }
  timed(GpuScope::kScene, [&] {
    BeginScenePass(ctx);
    mesh_pass_.Draw(ctx);
    skybox_pass_.Draw(ctx);
    reticle_pass_.Draw(ctx);
    EndScenePass(ctx);
  });
  if (ctx.view.post.bloom_enabled) {
    timed(GpuScope::kBloom, [&] { bloom_pass_.Record(ctx); });
  }
  timed(GpuScope::kTonemap, [&] { tonemap_pass_.Record(ctx); });
  if (OverlayPass::HasWork(ctx.view)) {
    timed(GpuScope::kOverlay, [&] { overlay_pass_.Record(ctx); });
  }
  if (output == Output::kSwapchain) {
    // Nothing to wait for but the acquire semaphore, which the submission
    // waits for at the color attachment output stage.
    const PresentTarget target{
        .image = ctx.swapchain_image,
        .view = ctx.swapchain_view,
        .extent = ctx.swapchain_extent,
        .previous_use = {VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_ACCESS_2_NONE},
    };
    timed(GpuScope::kPresent, [&] { present_pass_.Record(ctx, target); });
    timed(GpuScope::kUi, [&] { RecordUiPass(ctx, ui, UiBackground::kScene); });
  } else {
    // The previous frame's UI pass sampled the image.
    const PresentTarget target{
        .image = viewport_image_.handle(),
        .view = viewport_image_.view(),
        .extent = ToVkExtent(viewport_.extent),
        .previous_use = kFragmentSampledRead,
    };
    timed(GpuScope::kPresent, [&] {
      viewport_present_pass_->Record(ctx, target);
      RecordViewportReadbacks(cmd);
      ImageBarrier(cmd, target.image, VK_IMAGE_ASPECT_COLOR_BIT,
                   kColorAttachmentWrite, kFragmentSampledRead,
                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    });
    timed(GpuScope::kUi, [&] { RecordUiPass(ctx, ui, UiBackground::kClear); });
  }

  gpu_timer_.End(cmd, frame_index_, GpuScope::kTotal);
  CheckVk(vkEndCommandBuffer(cmd));
}

void RendererImpl::InitImGuiRenderer() {
  ui_.Init(context_, swapchain_);
  if (viewport_.generation != 0) {
    // PrepareViewport() ran before the backend existed.
    viewport_.texture = ui_.SetViewport(viewport_image_.view(), frame_serial_);
  }
}

void RendererImpl::ShutdownImGuiRenderer() {
  ui_.Shutdown();
  viewport_.texture = ImTextureID{};
}

Renderer::Renderer(const platform::Window& window,
                   const std::string& shader_dir,
                   shader::CompileMode shader_mode)
    : impl_(std::make_unique<RendererImpl>(window, shader_dir, shader_mode)) {}

Renderer::~Renderer() = default;

void Renderer::DrawFrame(const SceneView& view, ImDrawData* ui) {
  impl_->DrawFrameTo(view, ui, RendererImpl::Output::kSwapchain);
}

ViewportImage Renderer::PrepareViewport(core::Extent2D size) {
  return impl_->PrepareViewport(ToVkExtent(size));
}

const ViewportImage& Renderer::viewport() const { return impl_->viewport_; }

void Renderer::DrawViewportFrame(const SceneView& view, ImDrawData* ui) {
  if (impl_->viewport_.generation == 0) {
    core::log::Fatal("DrawViewportFrame() before PrepareViewport()");
  }
  impl_->DrawFrameTo(view, ui, RendererImpl::Output::kViewport);
}

uint64_t Renderer::RequestViewportReadback() {
  return impl_->RequestViewportReadback();
}

ReadbackStatus Renderer::TakeViewportReadback(uint64_t ticket,
                                              ViewportPixels& pixels) {
  return impl_->TakeViewportReadback(ticket, pixels);
}

MeshId Renderer::UploadMesh(const asset::MeshData& data) {
  return impl_->UploadMesh(data);
}

void Renderer::ReleaseMesh(MeshId id) { impl_->ReleaseMesh(id); }

void Renderer::ReleaseTextures(std::span<const TextureId> ids) {
  impl_->ReleaseTextures(ids);
}

void Renderer::ReleaseMaterial(MaterialId id) {
  impl_->materials_.Release(id);
}

ImTextureID Renderer::ui_texture(TextureId id) const {
  return impl_->ui_.Texture(id, impl_->textures_);
}

MaterialId Renderer::CreateMaterial(const material::MaterialDesc& desc) {
  return impl_->materials_.Create(desc);
}

std::vector<TextureId> Renderer::UploadTextures(
    std::span<const TextureUpload> textures) {
  return impl_->textures_.Upload(textures);
}

MaterialId Renderer::CreateMaterial(const material::MaterialDesc& desc,
                                    const MaterialTextures& textures) {
  return impl_->materials_.Create(desc, textures);
}

void Renderer::UpdateMaterial(MaterialId id,
                              const material::MaterialDesc& desc) {
  impl_->materials_.Update(id, desc);
}

const material::MaterialDesc& Renderer::material(MaterialId id) const {
  return impl_->materials_.desc(id);
}

size_t Renderer::material_count() const { return impl_->materials_.count(); }

TextureStats Renderer::texture_stats() const {
  return impl_->textures_.stats();
}

void Renderer::SetEnvironment(const asset::ImageDataF& equirect,
                              float intensity, float rotation_radians) {
  impl_->context_.WaitIdle();
  impl_->environment_.Set(equirect, intensity, rotation_radians);
}

void Renderer::ResetEnvironment() {
  impl_->context_.WaitIdle();
  impl_->environment_.Set({}, 1.0f, 0.0f);
}

void Renderer::SetEnvironmentParams(float intensity, float rotation_radians) {
  impl_->environment_.SetParams(intensity, rotation_radians);
}

void Renderer::RequestSwapchainRebuild() { impl_->swapchain_dirty_ = true; }

void Renderer::SetVsync(bool enabled) { impl_->SetVsync(enabled); }

void Renderer::ReloadChangedShaders() {
  impl_->shader_programs_.ReloadChanged();
}

std::string_view Renderer::last_shader_error() const {
  return impl_->shader_programs_.last_error();
}

bool Renderer::vsync() const { return impl_->vsync_; }

std::string_view Renderer::device_name() const {
  return impl_->context_.device_name();
}

core::Extent2D Renderer::output_extent() const {
  return ToExtent(impl_->swapchain_.extent());
}

void Renderer::WaitIdle() const { impl_->context_.WaitIdle(); }

std::optional<double> Renderer::gpu_frame_ms() const {
  return impl_->gpu_timer_.last_frame_ms();
}

std::optional<double> Renderer::gpu_scope_ms(GpuScope scope) const {
  return impl_->gpu_timer_.last_ms(scope);
}

RenderStats Renderer::stats() const { return impl_->stats_; }

void Renderer::InitImGuiRenderer() const { impl_->InitImGuiRenderer(); }

void Renderer::ShutdownImGuiRenderer() const {
  impl_->ShutdownImGuiRenderer();
}

}  // namespace blue::render
