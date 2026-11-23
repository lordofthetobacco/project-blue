#include "render/renderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glm/vec4.hpp>

#include "camera/frustum.h"
#include "camera/projection.h"
#include "core/log.h"
#include "lighting/cascades.h"
#include "material/material.h"
#include "platform/window.h"
#include "render/encoders.h"
#include "render/frame_uniforms.h"
#include "render/metal_check.h"
#include "render/passes/scene_pass.h"
#include "render/passes/ui_pass.h"
#include "render/renderer_impl.h"
#include "render/staging_upload.h"
#include "render/temporal_math.h"

namespace blue::render {

namespace {

// The editor viewport image: 8-bit RGBA with the sRGB encoding of the window
// output (BGRA8Unorm_sRGB), so the UI shows it as the game window would.
constexpr MTLPixelFormat kViewportFormat = MTLPixelFormatRGBA8Unorm_sRGB;

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

}  // namespace

RendererImpl::RendererImpl(const platform::Window& window,
                           const std::string& shader_dir,
                           shader::CompileMode shader_mode)
    : window_(window),
      shader_dir_(shader_dir),
      shader_mode_(shader_mode),
      context_(window),
      swapchain_(context_, window, window.PixelSize(), vsync_),
      samplers_(context_),
      targets_(context_, swapchain_.extent()),
      frame_globals_(context_, samplers_),
      textures_(context_),
      materials_(context_, textures_, samplers_),
      shader_programs_(context_, shader_mode_),
      environment_(context_, shader_programs_, shader_dir_, samplers_,
                   frame_globals_),
      shadow_pass_(context_, shader_programs_, shader_dir_, meshes_),
      depth_prepass_(context_, shader_programs_, shader_dir_, materials_,
                     meshes_),
      ssao_pass_(context_, shader_programs_, shader_dir_),
      mesh_pass_(context_, shader_programs_, shader_dir_, materials_, meshes_),
      skybox_pass_(context_, shader_programs_, shader_dir_, environment_),
      reticle_pass_(context_, shader_programs_, shader_dir_),
      scope_lens_pass_(context_, shader_programs_, shader_dir_),
      motion_blur_pass_(context_, shader_programs_, shader_dir_, meshes_),
      motion_vector_pass_(context_, shader_programs_, shader_dir_, meshes_),
      temporal_scaler_pass_(context_),
      bloom_pass_(context_, shader_programs_, shader_dir_),
      tonemap_pass_(context_, shader_programs_, shader_dir_),
      overlay_pass_(context_, shader_programs_, shader_dir_, samplers_,
                    meshes_),
      present_pass_(context_, shader_programs_, shader_dir_,
                    swapchain_.format()),
      gpu_timer_(context_) {
  const MaterialId default_material = materials_.Create(DefaultMaterialDesc());
  if (default_material != kDefaultMaterial) {
    core::log::Fatal("The default material must be created first");
  }
  for (Frame& frame : frames_) {
    frame.allocator = [context_.device() newCommandAllocator];
    FatalIfNil(frame.allocator, nil, "newCommandAllocator");
    frame.constants =
        std::make_unique<TransientArena>(context_, "Frame constants");
  }
  CreateFrameCommandBuffers();
  frame_globals_.BindShadowMap(shadow_pass_.map());
  BindTargets();
}

RendererImpl::~RendererImpl() { context_.WaitIdle(); }

void RendererImpl::CreateFrameCommandBuffers() {
  for (Frame& frame : frames_) {
    frame.command_buffer = [context_.device() newCommandBuffer];
    FatalIfNil(frame.command_buffer, nil, "newCommandBuffer");
    frame.command_buffer.label = @"frame";
  }
}

void RendererImpl::ConfigureTargets(core::Extent2D output, Upscaling mode) {
  if (!temporal_scaler_pass_.supported()) {
    mode = Upscaling::kOff;
  }
  const bool temporal = UsesTemporalScaler(mode);
  const core::Extent2D render =
      RenderExtent(output, mode, temporal_scaler_pass_.min_scale(),
                   temporal_scaler_pass_.max_scale());
  targets_.Resize(output, render, temporal);
  // A command buffer that recorded the scaler keeps listing the scaler's
  // own memory (MetalFX holds its history on a heap) even after it is begun
  // again; committing it once that scaler is gone crashes Metal's GPU
  // validation (MTL_SHADER_VALIDATION=1, -[MTL4GPUDebugCommandBuffer
  // preCommit:]). So the frame slots get new command buffers before the
  // scaler is replaced.
  CreateFrameCommandBuffers();
  temporal_scaler_pass_.Configure(targets_);
  BindTargets();
  upscaling_ = mode;
  temporal_history_valid_ = false;
}

void RendererImpl::BindTargets() {
  frame_globals_.BindAmbientOcclusion(targets_.ssao().texture());
  ssao_pass_.BindTargets(targets_, samplers_);
  motion_blur_pass_.BindTargets(targets_, samplers_);
  if (targets_.temporal()) {
    motion_vector_pass_.BindTargets(targets_, samplers_);
  }
  bloom_pass_.BindTargets(targets_, samplers_);
  tonemap_pass_.BindTargets(targets_, samplers_);
  overlay_pass_.BindTargets(targets_, samplers_);
  present_pass_.BindTargets(targets_, samplers_);
  if (viewport_present_pass_) {
    viewport_present_pass_->BindTargets(targets_, samplers_);
  }
}

MeshId RendererImpl::UploadMesh(std::span<const asset::Vertex> vertices,
                                std::span<const asset::Index> indices) {
  const Mesh& mesh = meshes_.emplace_back(context_, vertices, indices);
  mesh_bytes_ += mesh.size_bytes();
  mesh_count_ += 1;
  return static_cast<MeshId>(meshes_.size() - 1);
}

void RendererImpl::ReleaseMesh(MeshId id) {
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

void RendererImpl::ReleaseTextures(std::span<const TextureId> ids) {
  for (const TextureId id : ids) {
    // Retiring the image is also what keeps ui_texture(id), which does not
    // own the texture, valid for the frames in flight that drew it.
    Image image = textures_.TakeUploaded(id);
    if (image.texture() != nil) {
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
}

void RendererImpl::DestroyRetiredIfIdle() {
  if (retired_meshes_.empty() && retired_images_.empty()) {
    return;
  }
  for (const Frame& frame : frames_) {
    if (frame.submission != nullptr && !frame.submission->completed()) {
      return;
    }
  }
  retired_meshes_.clear();
  retired_images_.clear();
}

void RendererImpl::FlushReleases() {
  if (retired_meshes_.empty() && retired_images_.empty()) {
    return;
  }
  WaitForFramesInFlight();
  retired_meshes_.clear();
  retired_images_.clear();
}

MemoryStats RendererImpl::memory_stats() const {
  MemoryStats stats;
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
  const MetalContext::StagingUsage staging = context_.staging_usage();
  stats.staging_budget_bytes = 2 * StagingUploader::kChunkBytes;
  stats.staging_peak_bytes = staging.peak;
  stats.staging_current_bytes = staging.current;

  // Metal's one pool of GPU memory: what the process has allocated on the
  // device and what the system recommends as its limit.
  const id<MTLDevice> device = context_.device();
  const uint64_t allocated = device.currentAllocatedSize;
  const uint64_t recommended = device.recommendedMaxWorkingSetSize;
  stats.heap_count = 1;
  stats.driver_budget = true;
  stats.heaps[0] = {
      .heap_size = recommended,
      .device_local = true,
      .block_bytes = allocated,
      .allocation_bytes = stats.texture_bytes + stats.mesh_bytes +
                          stats.render_target_bytes + stats.environment_bytes +
                          stats.retired_bytes + stats.staging_current_bytes,
      .usage = allocated,
      .budget = recommended,
  };
  return stats;
}

void RendererImpl::SetVsync(bool enabled) {
  if (vsync_ != enabled) {
    vsync_ = enabled;
    swapchain_dirty_ = true;
  }
}

bool RendererImpl::RebuildSwapchain() {
  context_.WaitIdle();
  if (!swapchain_.Recreate(window_.PixelSize(), vsync_)) {
    return false;  // Surface has no area; stay dirty and retry next frame.
  }
  if (viewport_.generation == 0) {  // Otherwise they follow the viewport.
    ConfigureTargets(swapchain_.extent(), upscaling_);
  }
  swapchain_dirty_ = false;
  return true;
}

void RendererImpl::WaitForFrame(const Frame& frame) {
  if (frame.submission != nullptr) {
    frame.submission->Wait();
  }
}

void RendererImpl::WaitForFramesInFlight() const {
  for (const Frame& frame : frames_) {
    WaitForFrame(frame);
  }
}

ViewportImage RendererImpl::PrepareViewport(core::Extent2D size) {
  size = {std::max(size.width, 1u), std::max(size.height, 1u)};
  if (!viewport_present_pass_) {
    viewport_present_pass_.emplace(context_, shader_programs_, shader_dir_,
                                   kViewportFormat);
    viewport_present_pass_->BindTargets(targets_, samplers_);
  }
  const bool changed = size != viewport_.extent;
  const bool may_resize =
      viewport_.generation == 0 ||
      frame_serial_ >= viewport_resized_at_ + Renderer::kViewportResizeFrames;
  if (changed && may_resize) {
    ResizeViewport(size);
  }
  return viewport_;
}

void RendererImpl::ResizeViewport(core::Extent2D size) {
  WaitForFramesInFlight();
  // Frames up to the current one may have recorded the old image (the UI
  // samples it through a non-owning ImTextureID): retire it like a released
  // texture.
  if (viewport_image_.texture() != nil) {
    retired_images_.push_back(
        {.object = std::move(viewport_image_), .last_frame = frame_serial_});
  }
  viewport_image_ = Image(context_, {.extent = size,
                                     .format = kViewportFormat,
                                     .usage = MTLTextureUsageRenderTarget |
                                              MTLTextureUsageShaderRead,
                                     .label = "viewport"});
  ConfigureTargets(size, upscaling_);
  viewport_ = {
      .texture = ToImTextureId(viewport_image_.texture()),
      .extent = size,
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
  // A frame's command buffer is waited on kFramesInFlight frames later (as
  // for DestroyRetired()).
  if (it->frame_serial == 0 ||
      it->frame_serial + kFramesInFlight > frame_serial_) {
    return ReadbackStatus::kPending;
  }
  // Shared memory: the completed copy is visible without any flush.
  const auto* source = static_cast<const std::byte*>(it->buffer.mapped());
  pixels.width = it->extent.width;
  pixels.height = it->extent.height;
  pixels.rgba.assign(source, source + it->buffer.size());
  readbacks_.erase(it);
  return ReadbackStatus::kReady;
}

void RendererImpl::RecordViewportReadbacks(const FrameContext& ctx) {
  id<MTL4ComputeCommandEncoder> blit = nil;
  for (Readback& readback : readbacks_) {
    if (readback.frame_serial != 0) {
      continue;
    }
    if (blit == nil) {
      blit = BeginCopy(ctx.rec, "viewport readback");
    }
    readback.extent = viewport_image_.extent();
    readback.frame_serial = frame_serial_;
    const size_t width = readback.extent.width;
    const size_t height = readback.extent.height;
    const size_t bytes_per_row = width * 4;  // RGBA8, tightly packed.
    readback.buffer = Buffer(context_, bytes_per_row * height,
                             Buffer::Access::kCpuRead, "viewport readback");
    // Row 0 of the texture is its top row, so the rows land top first.
    [blit copyFromTexture:viewport_image_.texture()
                     sourceSlice:0
                     sourceLevel:0
                    sourceOrigin:MTLOriginMake(0, 0, 0)
                      sourceSize:MTLSizeMake(width, height, 1)
                        toBuffer:readback.buffer.handle()
               destinationOffset:0
          destinationBytesPerRow:bytes_per_row
        destinationBytesPerImage:bytes_per_row * height];
  }
  if (blit != nil) {
    [blit endEncoding];
  }
}

bool RendererImpl::AcquireDrawable(FrameContext& ctx) {
  ctx.drawable = swapchain_.NextDrawable();
  if (ctx.drawable == nil) {
    swapchain_dirty_ = true;
    return false;
  }
  return true;
}

void RendererImpl::DrawFrameTo(const SceneView& view, ImDrawData* ui,
                               Output output) {
  // Autoreleased Metal objects of the frame (descriptors, the drawable) are
  // freed at its end, not whenever the application's run loop drains.
  @autoreleasepool {
    // Before any early return: skipped frames must not hold released memory.
    DestroyRetiredIfIdle();
    // Until this frame is recorded the scaler's history is nobody's past (a
    // skipped frame breaks it).
    const bool history_valid =
        std::exchange(temporal_history_valid_, false);
    const core::Extent2D window_size = window_.PixelSize();
    if (window_size.width == 0 || window_size.height == 0) {
      return;  // Minimized: nothing to present to.
    }
    // The layer never reports its drawables as out of date: catch a resize
    // that was not requested.
    if (window_size != swapchain_.extent()) {
      swapchain_dirty_ = true;
    }
    if (swapchain_dirty_ && !RebuildSwapchain()) {
      return;
    }
    // A new upscaling mode needs other targets (and a scaler): like a
    // resize, once the frames in flight are done.
    const Upscaling wanted = temporal_scaler_pass_.supported()
                                 ? view.post.upscaling
                                 : Upscaling::kOff;
    if (wanted != upscaling_) {
      WaitForFramesInFlight();
      ConfigureTargets(targets_.output_extent(), wanted);
    }

    Frame& frame = frames_[frame_index_];
    WaitForFrame(frame);
    gpu_timer_.Collect(frame_index_, frame.submission.get());
    ++frame_serial_;
    materials_.BeginFrame(frame_serial_);
    DestroyRetired();

    // The slot's last work has completed: its drawable was presented, and
    // the memory of its commands and constants can be reused.
    frame.drawable = nil;
    [frame.allocator reset];
    frame.constants->Reset();
    [frame.command_buffer beginCommandBufferWithAllocator:frame.allocator];
    const Recording rec{.cmd = frame.command_buffer,
                        .args = context_.argument_table(),
                        .constants = *frame.constants};

    const bool shadows = view.shadows_enabled && view.sun.casts_shadows;
    std::array<lighting::Cascade, lighting::kCascadeCount> cascades{};
    const core::Extent2D extent = targets_.render_extent();
    const bool temporal = targets_.temporal();
    // The scene is rasterized with a different sub-pixel offset each frame;
    // culling, the cascades and the scope view never see it.
    const glm::vec2 jitter =
        temporal
            ? JitterOffset(temporal_frame_++,
                           JitterPhaseCount(extent, targets_.output_extent()))
            : glm::vec2(0.0f);
    // The scaler's history is this frame's past only if the frame before
    // was recorded and the app says the camera's history is good.
    const bool reset_history =
        !history_valid || !view.camera.previous_valid;
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
        MakeFrameUniforms(view, cascades, shadows, extent, jitter);
    environment_.FillUniforms(uniforms);
    frame_globals_.Write(frame_index_, uniforms);
    FrameContext ctx{
        .rec = rec,
        .frame_index = frame_index_,
        .globals = frame_globals_,
        .view = view,
        .targets = targets_,
        .temporal = temporal,
        .jitter = jitter,
        .scene_hdr = targets_.hdr(),
        .scene_depth = targets_.depth(),
        .scene_extent = extent,
        .output_extent = swapchain_.extent(),
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
      const uint32_t size =
          std::clamp(scope.resolution, 1u, kMaxScopeResolution);
      const core::Extent2D scope_extent{size, size};
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
          .rec = rec,
          .frame_index = frame_index_,
          .globals = frame_globals_,
          .frame_view = FrameView::kScope,
          .view = *scope_view,
          .targets = targets_,
          .scene_hdr = scope_picture_,
          .scene_depth = scope_depth_,
          .scene_extent = scope_extent,
          .world_bounds = world_bounds_,
          .camera_frustum = *scope_frustum,
          .cascades = cascades,
          .shadows = shadows,
          .draw_list = scope_draw_list_.entries(),
          .stats = scope_stats,
      });
    }
    RecordCommands(ctx, scope_ctx ? &*scope_ctx : nullptr, reset_history, ui,
                   output);
    temporal_history_valid_ = true;
    stats_.scope_draw_calls = scope_stats.draw_calls;
    [frame.command_buffer endCommandBuffer];
    // Presents the drawable (when one was acquired) through the queue.
    frame.drawable = ctx.drawable;
    frame.submission =
        context_.Commit(frame.command_buffer, "frame", frame.drawable);

    frame_index_ = (frame_index_ + 1) % kFramesInFlight;
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

void RendererImpl::EnsureScopeTargets() {
  if (scope_picture_.texture() != nil) {
    return;
  }
  const core::Extent2D extent{kMaxScopeResolution, kMaxScopeResolution};
  scope_picture_ = Image(context_, {.extent = extent,
                                    .format = RenderTargets::kHdrFormat,
                                    .usage = MTLTextureUsageRenderTarget |
                                             MTLTextureUsageShaderRead,
                                    .label = "Scope picture"});
  scope_depth_ = Image(context_, {.extent = extent,
                                  .format = RenderTargets::kDepthFormat,
                                  .usage = MTLTextureUsageRenderTarget,
                                  .label = "Scope depth"});
  // The lens may sample the picture before one was ever rendered (no light
  // through the scope): give it a defined, black content.
  MTL4RenderPassDescriptor* clear = [[MTL4RenderPassDescriptor alloc] init];
  clear.colorAttachments[0].texture = scope_picture_.texture();
  clear.colorAttachments[0].loadAction = MTLLoadActionClear;
  clear.colorAttachments[0].storeAction = MTLStoreActionStore;
  clear.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);
  context_.SubmitImmediate([clear](const Recording& immediate) {
    [BeginRenderPass(immediate, clear, "Clear scope picture") endEncoding];
  });
  scope_lens_pass_.BindPicture(scope_picture_.texture());
  core::log::Info("Scope picture target {} x {}", extent.width, extent.height);
}

void RendererImpl::RecordScopeView(const FrameContext& scope_ctx) {
  depth_prepass_.Record(scope_ctx);
  const id<MTL4RenderCommandEncoder> encoder = BeginScenePass(scope_ctx);
  mesh_pass_.Draw(scope_ctx, encoder);
  skybox_pass_.Draw(scope_ctx, encoder);
  EndScenePass(encoder);
}

void RendererImpl::DrawSights(const FrameContext& ctx,
                              id<MTL4RenderCommandEncoder> encoder) const {
  reticle_pass_.Draw(ctx, encoder);
  scope_lens_pass_.Draw(ctx, encoder);
}

void RendererImpl::RecordReactiveMask(const FrameContext& ctx) const {
  MTL4RenderPassDescriptor* descriptor =
      [[MTL4RenderPassDescriptor alloc] init];
  MTLRenderPassColorAttachmentDescriptor* mask = descriptor.colorAttachments[0];
  mask.texture = ctx.targets.reactive_mask().texture();
  mask.loadAction = MTLLoadActionClear;
  mask.storeAction = MTLStoreActionStore;
  mask.clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 0.0);
  MTLRenderPassDepthAttachmentDescriptor* depth = descriptor.depthAttachment;
  depth.texture = ctx.scene_depth.texture();
  depth.loadAction = MTLLoadActionLoad;
  depth.storeAction = MTLStoreActionStore;  // Tested only.

  const id<MTL4RenderCommandEncoder> encoder =
      BeginRenderPass(ctx.rec, descriptor, "reactive mask");
  SetSceneViewport(encoder, ctx.scene_extent);
  ctx.globals.Bind(ctx.rec, ctx.frame_index, ctx.frame_view);
  reticle_pass_.DrawReactive(ctx, encoder);
  scope_lens_pass_.DrawReactive(ctx, encoder);
  [encoder endEncoding];
}

void RendererImpl::RecordCommands(FrameContext& ctx,
                                  const FrameContext* scope_ctx,
                                  bool reset_history, ImDrawData* ui,
                                  Output output) {
  // Times `record` as `scope`.
  const auto timed = [&](GpuScope scope, const auto& record) {
    gpu_timer_.BeginScope(ctx.rec.cmd, scope);
    record();
    gpu_timer_.EndScope(ctx.rec.cmd, scope);
  };

  // Sharper mips for a scene rendered below the output size.
  const float lod_bias = TextureLodBias(ctx.targets.render_extent(),
                                        ctx.targets.output_extent());
  materials_.SetTextureLodBias(lod_bias);
  if (ctx.shadows) {
    timed(GpuScope::kShadow, [&] { shadow_pass_.Record(ctx); });
  }
  timed(GpuScope::kDepthPrepass, [&] { depth_prepass_.Record(ctx); });
  if (ctx.temporal) {
    timed(GpuScope::kMotionVectors, [&] { motion_vector_pass_.Record(ctx); });
  }
  if (ctx.view.post.ssao_enabled) {
    timed(GpuScope::kSsao, [&] { ssao_pass_.Record(ctx); });
  } else {
    ssao_pass_.Record(ctx);  // At most a one-time clear to white.
  }
  if (scope_ctx != nullptr) {
    // The scope picture is not upscaled: no bias for its materials.
    materials_.SetTextureLodBias(0.0f);
    timed(GpuScope::kScopeView, [&] { RecordScopeView(*scope_ctx); });
    materials_.SetTextureLodBias(lod_bias);
  }
  // The weapon motion blur rewrites the HDR image, so the sights are drawn
  // after it: the reflex dot and the scope's picture must stay sharp (the
  // picture is not weapon motion), and they are tonemapped once with the
  // rest of the image.
  const std::optional<MTLScissorRect> motion_blur_area =
      MotionBlurPass::FindArea(ctx);
  timed(GpuScope::kScene, [&] {
    const id<MTL4RenderCommandEncoder> encoder = BeginScenePass(ctx);
    mesh_pass_.Draw(ctx, encoder);
    skybox_pass_.Draw(ctx, encoder);
    if (!motion_blur_area) {
      DrawSights(ctx, encoder);
    }
    EndScenePass(encoder);
  });
  if (motion_blur_area) {
    timed(GpuScope::kMotionBlur, [&] {
      motion_blur_pass_.Record(ctx, *motion_blur_area);
      if (ctx.view.sight || ctx.view.scope) {
        const id<MTL4RenderCommandEncoder> encoder = ResumeScenePass(ctx);
        DrawSights(ctx, encoder);
        EndScenePass(encoder);
      }
    });
  }
  if (ctx.temporal) {
    // From here on the frame is at the output extent.
    timed(GpuScope::kUpscale, [&] {
      RecordReactiveMask(ctx);
      temporal_scaler_pass_.Record(ctx, reset_history);
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
    if (AcquireDrawable(ctx)) {
      timed(GpuScope::kPresent, [&] {
        present_pass_.Record(ctx, {.texture = ctx.drawable.texture,
                                   .extent = ctx.output_extent});
      });
      timed(GpuScope::kUi,
            [&] { RecordUiPass(ctx, ui, UiBackground::kScene); });
    }
    return;
  }

  timed(GpuScope::kPresent, [&] {
    viewport_present_pass_->Record(ctx, {.texture = viewport_image_.texture(),
                                         .extent = viewport_.extent});
    RecordViewportReadbacks(ctx);
  });
  if (AcquireDrawable(ctx)) {
    timed(GpuScope::kUi,
          [&] { RecordUiPass(ctx, ui, UiBackground::kClear, kUiClearBlack); });
  }
}

void RendererImpl::InitImGuiRenderer() { ui_.Init(context_); }

void RendererImpl::ShutdownImGuiRenderer() { ui_.Shutdown(); }

Renderer::Renderer(const platform::Window& window,
                   const std::string& shader_dir,
                   shader::CompileMode shader_mode)
    : impl_(std::make_unique<RendererImpl>(window, shader_dir, shader_mode)) {}

Renderer::~Renderer() = default;

void Renderer::DrawFrame(const SceneView& view, ImDrawData* ui) {
  impl_->DrawFrameTo(view, ui, RendererImpl::Output::kSwapchain);
}

ViewportImage Renderer::PrepareViewport(core::Extent2D size) {
  return impl_->PrepareViewport(size);
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

MeshId Renderer::UploadMesh(std::span<const asset::Vertex> vertices,
                            std::span<const asset::Index> indices) {
  return impl_->UploadMesh(vertices, indices);
}

void Renderer::ReleaseMesh(MeshId id) { impl_->ReleaseMesh(id); }

void Renderer::ReleaseTextures(std::span<const TextureId> ids) {
  impl_->ReleaseTextures(ids);
}

void Renderer::ReleaseMaterial(MaterialId id) { impl_->materials_.Release(id); }

void Renderer::FlushReleases() { impl_->FlushReleases(); }

ImTextureID Renderer::ui_texture(TextureId id) const {
  // Non-owning: the texture stays alive until ReleaseTextures() of `id` has
  // taken effect, because ReleaseTextures() retires the image like any other
  // GPU object (RendererImpl::retired_images_).
  return ToImTextureId(impl_->textures_.texture(id));
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

MemoryStats Renderer::memory_stats() const { return impl_->memory_stats(); }

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
  return impl_->swapchain_.extent();
}

core::Extent2D Renderer::render_extent() const {
  return impl_->targets_.render_extent();
}

core::Extent2D Renderer::scene_output_extent() const {
  return impl_->targets_.output_extent();
}

Upscaling Renderer::active_upscaling() const { return impl_->upscaling_; }

void Renderer::WaitIdle() const { impl_->context_.WaitIdle(); }

std::optional<double> Renderer::gpu_frame_ms() const {
  return impl_->gpu_timer_.last_frame_ms();
}

std::optional<double> Renderer::gpu_scope_ms(GpuScope scope) const {
  return impl_->gpu_timer_.last_ms(scope);
}

RenderStats Renderer::stats() const { return impl_->stats_; }

void Renderer::InitImGuiRenderer() const { impl_->InitImGuiRenderer(); }

void Renderer::ShutdownImGuiRenderer() const { impl_->ShutdownImGuiRenderer(); }

}  // namespace blue::render
