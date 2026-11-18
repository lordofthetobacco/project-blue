#include "render/renderer.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <glm/vec4.hpp>

#include "camera/frustum.h"
#include "core/log.h"
#include "lighting/cascades.h"
#include "material/material.h"
#include "platform/window.h"
#include "render/passes/scene_pass.h"
#include "render/passes/ui_pass.h"
#include "render/vk_check.h"

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

}  // namespace

Renderer::Renderer(const platform::Window& window,
                   const std::string& shader_dir,
                   shader::CompileMode shader_mode)
    : window_(window),
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
      bloom_pass_(context_, shader_programs_, shader_dir),
      tonemap_pass_(context_, shader_programs_, shader_dir),
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
  bloom_pass_.BindTargets(targets_, samplers_);
  tonemap_pass_.BindTargets(targets_, samplers_);
  present_pass_.BindTargets(targets_, samplers_);
}

MeshId Renderer::UploadMesh(const asset::MeshData& data) {
  meshes_.emplace_back(context_, data);
  return static_cast<MeshId>(meshes_.size() - 1);
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
  targets_.Resize(swapchain_.extent());
  BindTargets();
  swapchain_dirty_ = false;
  return true;
}

void Renderer::DrawFrame(const SceneView& view, ImDrawData* ui) {
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
  materials_.BeginFrame(frame_serial_);

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
  RecordCommands(frame, ctx, ui);
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

void Renderer::RecordCommands(const Frame& frame, const FrameContext& ctx,
                              ImDrawData* ui) {
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
  timed(GpuScope::kPresent, [&] { present_pass_.Record(ctx); });
  timed(GpuScope::kUi, [&] { RecordUiPass(ctx, ui); });

  gpu_timer_.End(cmd, frame_index_, GpuScope::kTotal);
  CheckVk(vkEndCommandBuffer(cmd));
}

}  // namespace blue::render
