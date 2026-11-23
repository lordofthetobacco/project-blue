#include "render/vulkan/vulkan_renderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <glm/vec4.hpp>

#include "camera/frustum.h"
#include "camera/projection.h"
#include "core/log.h"
#include "core/path.h"
#include "lighting/cascades.h"
#include "material/material.h"
#include "platform/window.h"
#include "render/frame_uniforms.h"
#include "render/vulkan/barriers.h"
#include "render/vulkan/passes/scene_pass.h"
#include "render/vulkan/passes/ui_pass.h"
#include "render/vulkan/staging_upload.h"
#include "render/vulkan/vk_check.h"
#include "render/vulkan/vk_extent.h"

namespace blue::render::vulkan {

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

// The headless output image's format: what the swapchain usually is.
constexpr VkFormat kHeadlessOutputFormat = VK_FORMAT_R8G8B8A8_SRGB;

// Returns the swapchain for `window`, or none when headless.
std::optional<Swapchain> MakeSwapchain(const VulkanContext& context,
                                       const platform::Window* window,
                                       bool vsync) {
  if (window == nullptr) {
    return std::nullopt;
  }
  return std::optional<Swapchain>(std::in_place, context,
                                  ToVkExtent(window->PixelSize()), vsync);
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

VulkanRenderer::VulkanRenderer(const RendererOptions& options,
                               std::unique_ptr<VulkanContext> context)
    : context_owner_(std::move(context)),
      context_(*context_owner_),
      window_(options.window),
      headless_extent_{std::max(options.headless_extent.width, 1u),
                       std::max(options.headless_extent.height, 1u)},
      swapchain_(MakeSwapchain(context_, options.window, vsync_)),
      samplers_(context_, options.max_anisotropy),
      targets_(context_, output_vk_extent()),
      frame_globals_(context_, samplers_),
      textures_(context_),
      materials_(context_, textures_, samplers_),
      shader_programs_(context_.device(),
                       core::JoinPath(options.shader_dir, "spirv"),
                       options.shader_source_dir, options.shader_mode),
      environment_(context_, shader_programs_, samplers_,
                   frame_globals_),
      shadow_pass_(context_, shader_programs_, meshes_),
      depth_prepass_(context_, shader_programs_,
                     frame_globals_.set_layout(), materials_, meshes_),
      ssao_pass_(context_, shader_programs_),
      mesh_pass_(context_, shader_programs_,
                 frame_globals_.set_layout(), materials_, meshes_),
      skybox_pass_(context_, shader_programs_,
                   frame_globals_.set_layout(), environment_),
      reticle_pass_(context_, shader_programs_,
                    frame_globals_.set_layout()),
      scope_lens_pass_(context_, shader_programs_,
                       frame_globals_.set_layout()),
      motion_blur_pass_(context_, shader_programs_,
                        frame_globals_.set_layout(), meshes_),
      bloom_pass_(context_, shader_programs_),
      tonemap_pass_(context_, shader_programs_),
      overlay_pass_(context_, shader_programs_,
                    frame_globals_.set_layout(), meshes_),
      present_pass_(context_, shader_programs_, output_format()),
      gpu_timer_(context_) {
  if (!swapchain_) {
    // Left in UNDEFINED: every frame's present pass discards it.
    output_image_ =
        Image(context_, {.extent = headless_extent_,
                         .format = kHeadlessOutputFormat,
                         .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                         .dedicated = true});
    core::log::Info("Headless output {} x {}", headless_extent_.width,
                    headless_extent_.height);
  }
  const MaterialId default_material = materials_.Create(DefaultMaterialDesc());
  if (default_material != kDefaultMaterial) {
    core::log::Fatal("The default material must be created first");
  }
  frame_globals_.BindShadowMap(shadow_pass_.map_view(), shadow_pass_.sampler());
  BindTargets();
  CreateFrames();
}

VulkanRenderer::~VulkanRenderer() {
  context_.WaitIdle();
  // ImGuiLayer shuts the backend down before; this covers a renderer
  // destroyed without it.
  ui_renderer_.Shutdown();

  const VkDevice device = context_.device();
  for (Frame& frame : frames_) {
    vkDestroySemaphore(device, frame.image_acquired, nullptr);
    vkDestroyFence(device, frame.in_flight, nullptr);
    vkDestroyCommandPool(device, frame.command_pool, nullptr);
  }
}

void VulkanRenderer::CreateFrames() {
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

void VulkanRenderer::BindTargets() {
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

MeshId VulkanRenderer::UploadMesh(std::span<const asset::Vertex> vertices,
                            std::span<const asset::Index> indices) {
  const Mesh& mesh = meshes_.emplace_back(context_, vertices, indices);
  mesh_bytes_ += mesh.size_bytes();
  mesh_count_ += 1;
  return static_cast<MeshId>(meshes_.size() - 1);
}

void VulkanRenderer::ReleaseMesh(MeshId id) {
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

void VulkanRenderer::ReleaseTextures(std::span<const TextureId> ids) {
  for (const TextureId id : ids) {
    ui_renderer_.Release(id, frame_serial_);
    Image image = textures_.TakeUploaded(id);
    if (image.handle() != VK_NULL_HANDLE) {
      retired_images_.push_back(
          {.object = std::move(image), .last_frame = frame_serial_});
    }
  }
}

void VulkanRenderer::DestroyRetired() {
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

void VulkanRenderer::DestroyRetiredIfIdle() {
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

void VulkanRenderer::FlushReleases() {
  if (retired_meshes_.empty() && retired_images_.empty()) {
    return;
  }
  WaitForFramesInFlight();
  retired_meshes_.clear();
  retired_images_.clear();
}

MemoryStats VulkanRenderer::memory_stats() const {
  MemoryStats stats;
  const VmaAllocator allocator = context_.allocator();
  const VkPhysicalDeviceMemoryProperties* properties = nullptr;
  vmaGetMemoryProperties(allocator, &properties);
  std::array<VmaBudget, VK_MAX_MEMORY_HEAPS> budgets{};
  vmaGetHeapBudgets(allocator, budgets.data());
  static_assert(kMaxMemoryHeaps >= VK_MAX_MEMORY_HEAPS);
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
      output_image_.size_bytes() +
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

void VulkanRenderer::SetVsync(bool enabled) {
  if (vsync_ != enabled) {
    vsync_ = enabled;
    swapchain_dirty_ = true;
  }
}

VkExtent2D VulkanRenderer::output_vk_extent() const {
  return swapchain_ ? swapchain_->extent() : headless_extent_;
}

core::Extent2D VulkanRenderer::output_extent() const {
  return ToExtent(output_vk_extent());
}

core::Extent2D VulkanRenderer::render_extent() const {
  return ToExtent(targets_.extent());
}

VkFormat VulkanRenderer::output_format() const {
  return swapchain_ ? swapchain_->format() : kHeadlessOutputFormat;
}

bool VulkanRenderer::RebuildSwapchain() {
  context_.WaitIdle();
  if (!swapchain_->Recreate(ToVkExtent(window_->PixelSize()), vsync_)) {
    return false;  // Surface has no area; stay dirty and retry next frame.
  }
  if (viewport_.generation == 0) {  // Otherwise they follow the viewport.
    targets_.Resize(swapchain_->extent());
    BindTargets();
  }
  swapchain_dirty_ = false;
  return true;
}

void VulkanRenderer::WaitForFramesInFlight() const {
  std::array<VkFence, kFramesInFlight> fences{};
  for (size_t i = 0; i < kFramesInFlight; ++i) {
    fences[i] = frames_[i].in_flight;
  }
  CheckVk(vkWaitForFences(context_.device(), kFramesInFlight, fences.data(),
                          VK_TRUE, UINT64_MAX));
}

ViewportImage VulkanRenderer::PrepareViewport(core::Extent2D requested) {
  const VkExtent2D size{std::max(requested.width, 1u),
                        std::max(requested.height, 1u)};
  if (!viewport_present_pass_) {
    viewport_present_pass_.emplace(context_, shader_programs_,
                                   ViewportFormat(output_format()));
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

void VulkanRenderer::ResizeViewport(VkExtent2D size) {
  WaitForFramesInFlight();
  viewport_image_ =
      Image(context_, {.extent = size,
                       .format = ViewportFormat(output_format()),
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
  // Frames up to the current one may have recorded the previous texture.
  viewport_ = {
      .texture =
          ui_renderer_.SetViewport(viewport_image_.view(), frame_serial_),
      .extent = ToExtent(size),
      .generation = viewport_.generation + 1};
  viewport_resized_at_ = frame_serial_;
  core::log::Info("Viewport output {} x {}", size.width, size.height);
}

void VulkanRenderer::DrawFrame(const SceneView& view, ImDrawData* ui) {
  DrawFrameTo(view, ui, Output::kSwapchain);
}

void VulkanRenderer::DrawViewportFrame(const SceneView& view, ImDrawData* ui) {
  if (viewport_.generation == 0) {
    core::log::Fatal("DrawViewportFrame() before PrepareViewport()");
  }
  DrawFrameTo(view, ui, Output::kViewport);
}

uint64_t VulkanRenderer::RequestViewportReadback() {
  const uint64_t ticket = next_readback_ticket_++;
  readbacks_.push_back({.ticket = ticket, .source = ReadbackSource::kViewport});
  return ticket;
}

uint64_t VulkanRenderer::RequestOutputReadback() {
  if (swapchain_ && !swapchain_->readable()) {
    core::log::Fatal("The swapchain images cannot be copied (no TRANSFER_SRC)");
  }
  const uint64_t ticket = next_readback_ticket_++;
  readbacks_.push_back({.ticket = ticket, .source = ReadbackSource::kOutput});
  return ticket;
}

ReadbackStatus VulkanRenderer::TakeViewportReadback(uint64_t ticket,
                                              ViewportPixels& pixels) {
  const auto it = std::ranges::find(readbacks_, ticket, &Readback::ticket);
  if (it == readbacks_.end()) {
    return ReadbackStatus::kUnknown;
  }
  if (it->frame_serial == 0) {
    return ReadbackStatus::kPending;
  }
  // The fence of a frame is waited on kFramesInFlight frames later (as for
  // DestroyRetired()); until then its slot was not reused, so its fence
  // tells whether the GPU is done.
  const bool done =
      it->frame_serial + kFramesInFlight <= frame_serial_ ||
      vkGetFenceStatus(context_.device(), frames_[it->frame_slot].in_flight) ==
          VK_SUCCESS;
  if (!done) {
    return ReadbackStatus::kPending;
  }
  it->buffer.InvalidateForRead();
  const auto* source = static_cast<const std::byte*>(it->buffer.mapped());
  pixels.width = it->extent.width;
  pixels.height = it->extent.height;
  pixels.rgba.assign(source, source + it->buffer.size());
  if (it->bgra) {
    for (size_t i = 0; i + 3 < pixels.rgba.size(); i += 4) {
      std::swap(pixels.rgba[i], pixels.rgba[i + 2]);
    }
  }
  readbacks_.erase(it);
  return ReadbackStatus::kReady;
}

void VulkanRenderer::RecordReadbacks(VkCommandBuffer cmd, ReadbackSource source,
                               const ReadbackImage& image) {
  const bool transition = image.layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  bool recorded = false;
  for (Readback& readback : readbacks_) {
    if (readback.frame_serial != 0 || readback.source != source) {
      continue;
    }
    if (!recorded && transition) {
      ImageBarrier(cmd, image.image, VK_IMAGE_ASPECT_COLOR_BIT,
                   image.last_write, kCopySourceRead, image.layout,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    }
    recorded = true;
    readback.extent = image.extent;
    readback.bgra = image.bgra;
    readback.frame_serial = frame_serial_;
    readback.frame_slot = frame_index_;
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
    vkCmdCopyImageToBuffer(cmd, image.image,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           readback.buffer.handle(), 1, &region);
  }
  if (!recorded) {
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
  if (transition) {
    ImageBarrier(cmd, image.image, VK_IMAGE_ASPECT_COLOR_BIT, kCopySourceRead,
                 image.last_write, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 image.layout);
  }
}

void VulkanRenderer::DrawFrameTo(const SceneView& view, ImDrawData* ui,
                           Output output) {
  // Before any early return: skipped frames must not hold released memory.
  DestroyRetiredIfIdle();
  if (swapchain_) {
    const core::Extent2D window_size = window_->PixelSize();
    if (window_size.width == 0 || window_size.height == 0) {
      return;  // Minimized: nothing to present to.
    }
    if (swapchain_dirty_ && !RebuildSwapchain()) {
      return;
    }
  }

  const Frame& frame = frames_[frame_index_];
  CheckVk(vkWaitForFences(context_.device(), 1, &frame.in_flight, VK_TRUE,
                          UINT64_MAX));
  gpu_timer_.Collect(frame_index_);

  // The swapchain image; headless, the output image stands in for it.
  std::optional<uint32_t> image_index;
  if (swapchain_) {
    image_index = AcquireImage(frame);
    if (!image_index) {
      return;
    }
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
  ui_renderer_.FreeRetired(frame_serial_);

  const FramePlan plan = MakeFramePlan(view, caps());
  const bool shadows = plan.shadows;
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

  draw_list_.Build(view, world_bounds_, camera_frustum,
                   draw_source_.resources(), stats_);
  FrameUniforms uniforms =
      MakeFrameUniforms(view, cascades, shadows, ToExtent(extent));
  environment_.FillUniforms(uniforms);
  frame_globals_.Write(frame_index_, uniforms);
  const FrameContext ctx{
      .cmd = frame.command_buffer,
      .frame_index = frame_index_,
      .frame_set = frame_globals_.set(frame_index_),
      .view = view,
      .plan = plan,
      .targets = targets_,
      .scene_hdr = targets_.hdr(),
      .scene_depth = targets_.depth(),
      .scene_extent = extent,
      .swapchain_image = image_index ? swapchain_->image(*image_index)
                                     : output_image_.handle(),
      .swapchain_view = image_index ? swapchain_->image_view(*image_index)
                                    : output_image_.view(),
      .swapchain_extent = output_vk_extent(),
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
  if (plan.scope) {
    EnsureScopeTargets();
  }
  if (plan.scope_picture) {
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
                           draw_source_.resources(), scope_stats,
                           /*scope_view=*/true);
    FrameUniforms scope_uniforms = MakeFrameUniforms(
        *scope_view, cascades, shadows, ToExtent(scope_extent));
    environment_.FillUniforms(scope_uniforms);
    frame_globals_.Write(frame_index_, scope_uniforms, FrameView::kScope);
    scope_ctx.emplace(FrameContext{
        .cmd = frame.command_buffer,
        .frame_index = frame_index_,
        .frame_set = frame_globals_.set(frame_index_, FrameView::kScope),
        .view = *scope_view,
        .plan = plan,
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
  SubmitAndPresent(frame, image_index);

  frame_index_ = (frame_index_ + 1) % kFramesInFlight;
}

std::optional<uint32_t> VulkanRenderer::AcquireImage(const Frame& frame) {
  uint32_t image_index = 0;
  const VkResult result =
      vkAcquireNextImageKHR(context_.device(), swapchain_->handle(), UINT64_MAX,
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

void VulkanRenderer::SubmitAndPresent(const Frame& frame,
                                std::optional<uint32_t> image_index) {
  const VkCommandBufferSubmitInfo cmd_info{
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
      .commandBuffer = frame.command_buffer,
  };
  if (!image_index) {  // Headless: nothing to acquire or present.
    const VkSubmitInfo2 submit_info{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .commandBufferInfoCount = 1,
        .pCommandBufferInfos = &cmd_info,
    };
    CheckVk(vkQueueSubmit2(context_.queue(), 1, &submit_info, frame.in_flight));
    return;
  }

  const VkSemaphore render_finished = swapchain_->render_finished(*image_index);
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

  const VkSwapchainKHR swapchain = swapchain_->handle();
  const VkPresentInfoKHR present_info{
      .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
      .waitSemaphoreCount = 1,
      .pWaitSemaphores = &render_finished,
      .swapchainCount = 1,
      .pSwapchains = &swapchain,
      .pImageIndices = &*image_index,
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

void VulkanRenderer::ComputeWorldBounds(const SceneView& view) {
  world_bounds_.clear();
  world_bounds_.reserve(view.draws.size());
  for (const DrawItem& item : view.draws) {
    world_bounds_.push_back(camera::TransformAabb(
        {.min = item.bounds.min, .max = item.bounds.max}, item.transform));
  }
}

void VulkanRenderer::EnsureScopeTargets() {
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

void VulkanRenderer::RecordScopeView(const FrameContext& scope_ctx) {
  depth_prepass_.Record(scope_ctx);
  BeginScenePass(scope_ctx);
  mesh_pass_.Draw(scope_ctx);
  skybox_pass_.Draw(scope_ctx);
  EndScenePass(scope_ctx);
}

void VulkanRenderer::DrawSights(const FrameContext& ctx) const {
  reticle_pass_.Draw(ctx);
  scope_lens_pass_.Draw(ctx);
}

void VulkanRenderer::RecordCommands(const Frame& frame, const FrameContext& ctx,
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
  if (ctx.plan.ssao) {
    timed(GpuScope::kSsao, [&] { ssao_pass_.Record(ctx); });
  } else {
    ssao_pass_.Record(ctx);  // At most a one-time clear to white.
  }
  if (scope_ctx != nullptr) {  // FramePlan::scope_picture.
    timed(GpuScope::kScopeView, [&] { RecordScopeView(*scope_ctx); });
  }
  // The weapon motion blur rewrites the HDR image, so the sights are drawn
  // after it: the reflex dot and the scope's picture must stay sharp (the
  // picture is not weapon motion), and they are tonemapped once with the
  // rest of the image.
  const std::optional<VkRect2D> motion_blur_area =
      ctx.plan.motion_blur ? MotionBlurPass::FindArea(ctx) : std::nullopt;
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
  if (ctx.plan.bloom) {
    timed(GpuScope::kBloom, [&] { bloom_pass_.Record(ctx); });
  }
  timed(GpuScope::kTonemap, [&] { tonemap_pass_.Record(ctx); });
  if (ctx.plan.overlay) {
    timed(GpuScope::kOverlay, [&] { overlay_pass_.Record(ctx); });
  }
  // Window: the swapchain image was just acquired (only the acquire
  // semaphore to wait for); headless: the output image was last copied.
  const UiOutputUse& output_use = swapchain_ ? kUiPresent : kUiCopy;
  if (output == Output::kSwapchain) {
    const PresentTarget target{
        .image = ctx.swapchain_image,
        .view = ctx.swapchain_view,
        .extent = ctx.swapchain_extent,
        .previous_use = output_use.previous_use,
    };
    timed(GpuScope::kPresent, [&] { present_pass_.Record(ctx, target); });
    timed(GpuScope::kUi, [&] {
      RecordUiPass(ctx, ui, UiBackground::kScene, output_use);
    });
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
      RecordReadbacks(cmd, ReadbackSource::kViewport,
                      {.image = target.image,
                       .extent = target.extent,
                       .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                       .last_write = kColorAttachmentWrite});
      ImageBarrier(cmd, target.image, VK_IMAGE_ASPECT_COLOR_BIT,
                   kColorAttachmentWrite, kFragmentSampledRead,
                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    });
    timed(GpuScope::kUi, [&] {
      RecordUiPass(ctx, ui, UiBackground::kClear, output_use);
    });
  }
  const VkFormat format = output_format();
  RecordReadbacks(cmd, ReadbackSource::kOutput,
                  {.image = ctx.swapchain_image,
                   .extent = ctx.swapchain_extent,
                   .layout = output_use.final_layout,
                   .last_write = kColorAttachmentWrite,
                   .bgra = format == VK_FORMAT_B8G8R8A8_SRGB ||
                           format == VK_FORMAT_B8G8R8A8_UNORM});

  gpu_timer_.End(cmd, frame_index_, GpuScope::kTotal);
  CheckVk(vkEndCommandBuffer(cmd));
}

void VulkanRenderer::InitImGuiRenderer() const {
  // Headless, the output image stands in for a swapchain of two images.
  constexpr uint32_t kHeadlessImageCount = 2;
  ui_renderer_.Init(
      context_, output_format(),
      swapchain_ ? swapchain_->min_image_count() : kHeadlessImageCount,
      swapchain_ ? swapchain_->image_count() : kHeadlessImageCount);
  if (viewport_.generation != 0) {
    // The viewport image existed before the backend; frames recorded
    // until now could not show it.
    viewport_.texture =
        ui_renderer_.SetViewport(viewport_image_.view(), frame_serial_);
  }
}

void VulkanRenderer::ShutdownImGuiRenderer() const {
  ui_renderer_.Shutdown();
  viewport_.texture = ImTextureID{};
}

}  // namespace blue::render::vulkan

namespace blue::render {

std::unique_ptr<RendererBackend> CreateVulkanRenderer(
    const RendererOptions& options) {
  // Debug builds validate whatever options.gpu_validation says.
  auto context = std::make_unique<vulkan::VulkanContext>(
      options.window, options.adapter, options.gpu_validation);
  if (!context->has_device()) {
    return nullptr;
  }
  return std::make_unique<vulkan::VulkanRenderer>(options, std::move(context));
}

}  // namespace blue::render
