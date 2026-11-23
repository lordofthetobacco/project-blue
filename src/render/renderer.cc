#include "render/renderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <glm/vec4.hpp>

#include "camera/frustum.h"
#include "camera/projection.h"
#include "core/log.h"
#include "lighting/cascades.h"
#include "material/material.h"
#include "platform/window.h"
#include "render/barriers.h"
#include "render/passes/scene_pass.h"
#include "render/passes/ui_pass.h"
#include "render/staging_upload.h"
#include "render/vk_check.h"

namespace blue::render {

namespace {

// m, the scope camera's near plane: nothing of the world is that close to
// the objective.
constexpr float kScopeNearPlane = 0.05f;

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

Renderer::Renderer(const platform::Window& window,
                   const std::string& shader_dir,
                   shader::CompileMode shader_mode)
    : window_(window),
      shader_dir_(shader_dir),
      context_(window),
      swapchain_(context_, window.PixelSize(), vsync_),
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
      scope_lens_pass_(context_, shader_programs_, shader_dir,
                       frame_globals_.set_layout()),
      motion_blur_pass_(context_, shader_programs_, shader_dir,
                        frame_globals_.set_layout(), meshes_),
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

Renderer::~Renderer() {
  context_.WaitIdle();

  const VkDevice device = context_.device();
  for (Frame& frame : frames_) {
    vkDestroySemaphore(device, frame.image_acquired, nullptr);
    vkDestroyFence(device, frame.in_flight, nullptr);
    vkDestroyCommandPool(device, frame.command_pool, nullptr);
  }
}

void Renderer::CreateFrames() {
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

void Renderer::BindTargets() {
  frame_globals_.BindAmbientOcclusion(targets_.ssao().view());
  ssao_pass_.BindTargets(targets_, samplers_);
  motion_blur_pass_.BindTargets(targets_, samplers_);
  bloom_pass_.BindTargets(targets_, samplers_);
  tonemap_pass_.BindTargets(targets_, samplers_);
  overlay_pass_.BindTargets(targets_, samplers_);
  present_pass_.BindTargets(targets_, samplers_);
  if (viewport_present_pass_) {
    viewport_present_pass_->BindTargets(targets_, samplers_);
  }
}

MeshId Renderer::UploadMesh(std::span<const asset::Vertex> vertices,
                            std::span<const asset::Index> indices) {
  const Mesh& mesh = meshes_.emplace_back(context_, vertices, indices);
  mesh_bytes_ += mesh.size_bytes();
  mesh_count_ += 1;
  return static_cast<MeshId>(meshes_.size() - 1);
}

void Renderer::ReleaseMesh(MeshId id) {
  const auto index = static_cast<size_t>(id);
  if (index >= meshes_.size() || !meshes_[index].resident()) {
    return;
  }
  mesh_bytes_ -= meshes_[index].size_bytes();
  mesh_count_ -= 1;
  // The moved-from mesh left in the slot is the tombstone (resident() false).
  retired_meshes_.push_back(
      {.object = std::move(meshes_[index]), .last_frame = frame_serial_});
}

void Renderer::ReleaseTextures(std::span<const TextureId> ids) {
  for (const TextureId id : ids) {
    Image image = textures_.TakeUploaded(id);
    if (image.handle() != VK_NULL_HANDLE) {
      retired_images_.push_back(
          {.object = std::move(image), .last_frame = frame_serial_});
    }
  }
}

void Renderer::DestroyRetired() {
  const auto done = [this](uint64_t last_frame) {
    return last_frame + kFramesInFlight <= frame_serial_;
  };
  std::erase_if(retired_meshes_, [&](const Retired<Mesh>& retired) {
    return done(retired.last_frame);
  });
  std::erase_if(retired_images_, [&](const Retired<Image>& retired) {
    return done(retired.last_frame);
  });
}

void Renderer::DestroyRetiredIfIdle() {
  if (retired_meshes_.empty() && retired_images_.empty()) {
    return;
  }
  // A frame's fence is reset only right before its work is submitted, so
  // all fences signalled means no submitted frame can still use anything.
  for (const Frame& frame : frames_) {
    if (vkGetFenceStatus(context_.device(), frame.in_flight) != VK_SUCCESS) {
      return;
    }
  }
  retired_meshes_.clear();
  retired_images_.clear();
}

void Renderer::FlushReleases() {
  if (retired_meshes_.empty() && retired_images_.empty()) {
    return;
  }
  WaitForFramesInFlight();
  retired_meshes_.clear();
  retired_images_.clear();
}

MemoryStats Renderer::memory_stats() const {
  MemoryStats stats;
  const VmaAllocator allocator = context_.allocator();
  const VkPhysicalDeviceMemoryProperties* properties = nullptr;
  vmaGetMemoryProperties(allocator, &properties);
  std::array<VmaBudget, VK_MAX_MEMORY_HEAPS> budgets{};
  vmaGetHeapBudgets(allocator, budgets.data());
  stats.heap_count = properties->memoryHeapCount;
  stats.driver_budget = context_.memory_budget();
  for (uint32_t i = 0; i < stats.heap_count; ++i) {
    const VmaBudget& budget = budgets[i];
    stats.heaps[i] = {
        .heap_size = properties->memoryHeaps[i].size,
        .device_local = (properties->memoryHeaps[i].flags &
                         VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0,
        .block_bytes = budget.statistics.blockBytes,
        .allocation_bytes = budget.statistics.allocationBytes,
        .block_count = budget.statistics.blockCount,
        .allocation_count = budget.statistics.allocationCount,
        .usage = budget.usage,
        .budget = budget.budget,
    };
  }

  const TextureStats textures = textures_.stats();
  stats.texture_bytes = textures.bytes;
  stats.texture_count = static_cast<uint32_t>(textures.count);
  stats.mesh_bytes = mesh_bytes_;
  stats.mesh_count = mesh_count_;
  stats.render_target_bytes =
      targets_.size_bytes() + viewport_image_.size_bytes() +
      shadow_pass_.map_bytes() + scope_picture_.size_bytes() +
      scope_depth_.size_bytes();
  stats.environment_bytes = environment_.size_bytes();
  // Usually empty, a handful of entries right after a release.
  for (const Retired<Mesh>& retired : retired_meshes_) {
    stats.retired_bytes += retired.object.size_bytes();
  }
  for (const Retired<Image>& retired : retired_images_) {
    stats.retired_bytes += retired.object.size_bytes();
  }
  const VulkanContext::StagingUsage staging = context_.staging_usage();
  stats.staging_budget_bytes = 2 * StagingUploader::kChunkBytes;
  stats.staging_peak_bytes = staging.peak;
  stats.staging_current_bytes = staging.current;
  return stats;
}

void Renderer::SetVsync(bool enabled) {
  if (vsync_ != enabled) {
    vsync_ = enabled;
    swapchain_dirty_ = true;
  }
}

bool Renderer::RebuildSwapchain() {
  context_.WaitIdle();
  if (!swapchain_.Recreate(window_.PixelSize(), vsync_)) {
    return false;  // Surface has no area; stay dirty and retry next frame.
  }
  if (viewport_.generation == 0) {  // Otherwise they follow the viewport.
    targets_.Resize(swapchain_.extent());
    BindTargets();
  }
  swapchain_dirty_ = false;
  return true;
}

void Renderer::WaitForFramesInFlight() const {
  std::array<VkFence, kFramesInFlight> fences{};
  for (size_t i = 0; i < kFramesInFlight; ++i) {
    fences[i] = frames_[i].in_flight;
  }
  CheckVk(vkWaitForFences(context_.device(), kFramesInFlight, fences.data(),
                          VK_TRUE, UINT64_MAX));
}

ViewportImage Renderer::PrepareViewport(VkExtent2D size) {
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
      frame_serial_ >= viewport_resized_at_ + kViewportResizeFrames;
  if (changed && may_resize) {
    ResizeViewport(size);
  }
  return viewport_;
}

void Renderer::ResizeViewport(VkExtent2D size) {
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
  viewport_ = {.view = viewport_image_.view(),
               .extent = size,
               .generation = viewport_.generation + 1};
  viewport_resized_at_ = frame_serial_;
  core::log::Info("Viewport output {} x {}", size.width, size.height);
}

void Renderer::DrawFrame(const SceneView& view, ImDrawData* ui) {
  DrawFrameTo(view, ui, Output::kSwapchain);
}

void Renderer::DrawViewportFrame(const SceneView& view, ImDrawData* ui) {
  if (viewport_.generation == 0) {
    core::log::Fatal("DrawViewportFrame() before PrepareViewport()");
  }
  DrawFrameTo(view, ui, Output::kViewport);
}

uint64_t Renderer::RequestViewportReadback() {
  const uint64_t ticket = next_readback_ticket_++;
  readbacks_.push_back({.ticket = ticket});
  return ticket;
}

ReadbackStatus Renderer::TakeViewportReadback(uint64_t ticket,
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

void Renderer::RecordViewportReadbacks(VkCommandBuffer cmd) {
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
    readback.extent = viewport_.extent;
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

void Renderer::DrawFrameTo(const SceneView& view, ImDrawData* ui,
                           Output output) {
  // Before any early return: skipped frames must not hold released memory.
  DestroyRetiredIfIdle();
  const VkExtent2D window_size = window_.PixelSize();
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
  // Lets VMA refresh the driver's memory budget once per frame.
  vmaSetCurrentFrameIndex(context_.allocator(),
                          static_cast<uint32_t>(frame_serial_));
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
  FrameUniforms uniforms = MakeFrameUniforms(view, cascades, shadows, extent);
  environment_.FillUniforms(uniforms);
  frame_globals_.Write(frame_index_, uniforms);
  const FrameContext ctx{
      .cmd = frame.command_buffer,
      .frame_index = frame_index_,
      .frame_set = frame_globals_.set(frame_index_),
      .view = view,
      .targets = targets_,
      .scene_hdr = targets_.hdr(),
      .scene_depth = targets_.depth(),
      .scene_extent = extent,
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

  // The scope's picture: the same scene from the scope's camera, culled by
  // its own frustum, into the corner of the scope target its size needs.
  std::optional<SceneView> scope_view;
  std::optional<camera::Frustum> scope_frustum;
  std::optional<FrameContext> scope_ctx;
  RenderStats scope_stats;
  if (view.scope) {
    EnsureScopeTargets();
  }
  if (view.scope && view.scope->render_picture) {
    const ScopeSight& scope = *view.scope;
    const uint32_t size = std::clamp(scope.resolution, 1u, kMaxScopeResolution);
    const VkExtent2D scope_extent{size, size};
    scope_view.emplace(view);
    scope_view->scope.reset();
    scope_view->sight.reset();
    scope_view->post.weapon_motion_blur_enabled = false;
    Camera& camera = scope_view->camera;
    camera.view = scope.view;
    camera.position = scope.position;
    camera.vertical_fov_radians =
        2.0f * std::atan(std::max(scope.camera_tangent, 1.0e-5f));
    camera.near_plane = kScopeNearPlane;
    camera.projection = camera::ReversedInfinitePerspective(
        camera.vertical_fov_radians, 1.0f, kScopeNearPlane);
    camera.previous_view_projection = camera.projection * camera.view;
    scope_frustum.emplace(camera.projection * camera.view);
    scope_draw_list_.Build(*scope_view, world_bounds_, *scope_frustum,
                           materials_, meshes_, scope_stats,
                           /*scope_view=*/true);
    FrameUniforms scope_uniforms =
        MakeFrameUniforms(*scope_view, cascades, shadows, scope_extent);
    environment_.FillUniforms(scope_uniforms);
    frame_globals_.Write(frame_index_, scope_uniforms, FrameView::kScope);
    scope_ctx.emplace(FrameContext{
        .cmd = frame.command_buffer,
        .frame_index = frame_index_,
        .frame_set = frame_globals_.set(frame_index_, FrameView::kScope),
        .view = *scope_view,
        .targets = targets_,
        .scene_hdr = scope_picture_,
        .scene_depth = scope_depth_,
        .scene_extent = scope_extent,
        .swapchain_image = VK_NULL_HANDLE,
        .swapchain_view = VK_NULL_HANDLE,
        .swapchain_extent = {},
        .world_bounds = world_bounds_,
        .camera_frustum = *scope_frustum,
        .cascades = cascades,
        .shadows = shadows,
        .draw_list = scope_draw_list_.entries(),
        .stats = scope_stats,
    });
  }
  RecordCommands(frame, ctx, scope_ctx ? &*scope_ctx : nullptr, ui, output);
  stats_.scope_draw_calls = scope_stats.draw_calls;
  SubmitAndPresent(frame, *image_index);

  frame_index_ = (frame_index_ + 1) % kFramesInFlight;
}

std::optional<uint32_t> Renderer::AcquireImage(const Frame& frame) {
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

void Renderer::SubmitAndPresent(const Frame& frame, uint32_t image_index) {
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

void Renderer::ComputeWorldBounds(const SceneView& view) {
  world_bounds_.clear();
  world_bounds_.reserve(view.draws.size());
  for (const DrawItem& item : view.draws) {
    world_bounds_.push_back(camera::TransformAabb(
        {.min = item.bounds.min, .max = item.bounds.max}, item.transform));
  }
}

void Renderer::EnsureScopeTargets() {
  if (scope_picture_.handle() != VK_NULL_HANDLE) {
    return;
  }
  const VkExtent2D extent{kMaxScopeResolution, kMaxScopeResolution};
  scope_picture_ =
      Image(context_, {.extent = extent,
                       .format = RenderTargets::kHdrFormat,
                       .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                VK_IMAGE_USAGE_SAMPLED_BIT |
                                VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                       .dedicated = true});
  scope_depth_ =
      Image(context_, {.extent = extent,
                       .format = RenderTargets::kDepthFormat,
                       .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                       .aspect = VK_IMAGE_ASPECT_DEPTH_BIT,
                       .dedicated = true});
  // The lens may sample the picture before one was ever rendered (no light
  // through the scope): give it a defined, black content.
  context_.SubmitImmediate([this](VkCommandBuffer cmd) {
    constexpr BarrierScope kTransferWrite{VK_PIPELINE_STAGE_2_CLEAR_BIT,
                                          VK_ACCESS_2_TRANSFER_WRITE_BIT};
    ImageBarrier(cmd, scope_picture_.handle(), VK_IMAGE_ASPECT_COLOR_BIT,
                 kNoAccess, kTransferWrite, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const VkClearColorValue black{{0.0f, 0.0f, 0.0f, 1.0f}};
    const VkImageSubresourceRange range = WholeImage(VK_IMAGE_ASPECT_COLOR_BIT);
    vkCmdClearColorImage(cmd, scope_picture_.handle(),
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1,
                         &range);
    ImageBarrier(cmd, scope_picture_.handle(), VK_IMAGE_ASPECT_COLOR_BIT,
                 kTransferWrite, kFragmentSampledRead,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  });
  scope_lens_pass_.BindPicture(scope_picture_.view(), samplers_.linear_clamp());
  core::log::Info("Scope picture target {} x {}", extent.width, extent.height);
}

void Renderer::RecordScopeView(const FrameContext& scope_ctx) {
  depth_prepass_.Record(scope_ctx);
  BeginScenePass(scope_ctx);
  mesh_pass_.Draw(scope_ctx);
  skybox_pass_.Draw(scope_ctx);
  EndScenePass(scope_ctx);
}

void Renderer::DrawSights(const FrameContext& ctx) const {
  reticle_pass_.Draw(ctx);
  scope_lens_pass_.Draw(ctx);
}

void Renderer::RecordCommands(const Frame& frame, const FrameContext& ctx,
                              const FrameContext* scope_ctx, ImDrawData* ui,
                              Output output) {
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
  if (scope_ctx != nullptr) {
    timed(GpuScope::kScopeView, [&] { RecordScopeView(*scope_ctx); });
  }
  // The weapon motion blur rewrites the HDR image, so the sights are drawn
  // after it: the reflex dot and the scope's picture must stay sharp (the
  // picture is not weapon motion), and they are tonemapped once with the
  // rest of the image.
  const std::optional<VkRect2D> motion_blur_area =
      MotionBlurPass::FindArea(ctx);
  timed(GpuScope::kScene, [&] {
    BeginScenePass(ctx);
    mesh_pass_.Draw(ctx);
    skybox_pass_.Draw(ctx);
    if (!motion_blur_area) {
      DrawSights(ctx);
    }
    EndScenePass(ctx);
  });
  if (motion_blur_area) {
    timed(GpuScope::kMotionBlur, [&] {
      motion_blur_pass_.Record(ctx, *motion_blur_area);
      if (ctx.view.sight || ctx.view.scope) {
        ResumeScenePass(ctx);
        DrawSights(ctx);
        EndScenePass(ctx);
      }
    });
  }
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
        .extent = viewport_.extent,
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

}  // namespace blue::render
