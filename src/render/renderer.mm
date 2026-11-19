#include "render/renderer.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <glm/vec4.hpp>

#include "core/log.h"
#include "core/path.h"
#include "material/material.h"
#include "platform/window.h"
#include "render/encoders.h"
#include "render/metal_check.h"
#include "render/passes/ui_pass.h"
#include "render/renderer_impl.h"

namespace blue::render {

namespace {

// The editor viewport image: 8-bit RGBA with the sRGB encoding of the window
// output (BGRA8Unorm_sRGB), so the UI shows it as the game window would.
constexpr MTLPixelFormat kViewportFormat = MTLPixelFormatRGBA8Unorm_sRGB;

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
      textures_(context_),
      materials_(context_, textures_, samplers_),
      shader_programs_(context_, shader_mode_),
      debug_gradient_pass_(
          context_, shader_programs_, shader_dir_,
          {.fragment_path =
               core::JoinPath(shader_dir_, "debug_gradient.frag.metal"),
           .input_count = 0,
           .output_format = RenderTargets::kHdrFormat,
           .label = "debug gradient"}),
      bloom_pass_(context_, shader_programs_, shader_dir_),
      tonemap_pass_(context_, shader_programs_, shader_dir_),
      present_pass_(context_, shader_programs_, shader_dir_,
                    swapchain_.format()) {
  const MaterialId default_material = materials_.Create(DefaultMaterialDesc());
  if (default_material != kDefaultMaterial) {
    core::log::Fatal("The default material must be created first");
  }
  BindTargets();
}

RendererImpl::~RendererImpl() { context_.WaitIdle(); }

void RendererImpl::BindTargets() {
  bloom_pass_.BindTargets(targets_, samplers_);
  tonemap_pass_.BindTargets(targets_, samplers_);
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
    targets_.Resize(swapchain_.extent());
    BindTargets();
  }
  swapchain_dirty_ = false;
  return true;
}

void RendererImpl::WaitForFrame(const Frame& frame) {
  if (frame.command_buffer == nil) {
    return;  // Nothing recorded into this slot yet.
  }
  [frame.command_buffer waitUntilCompleted];
  CheckCommandBuffer(frame.command_buffer);
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
  targets_.Resize(size);
  BindTargets();
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
  id<MTLBlitCommandEncoder> blit = nil;
  for (Readback& readback : readbacks_) {
    if (readback.frame_serial != 0) {
      continue;
    }
    if (blit == nil) {
      blit = BeginBlit(ctx, "viewport readback");
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

    Frame& frame = frames_[frame_index_];
    WaitForFrame(frame);
    gpu_timer_.Collect(frame.command_buffer);
    ++frame_serial_;
    materials_.BeginFrame(frame_serial_);
    DestroyRetired();

    // Retains every resource it uses until it has completed.
    frame.command_buffer = [context_.queue() commandBuffer];
    FatalIfNil(frame.command_buffer, nil, "commandBuffer");
    frame.command_buffer.label = @"frame";

    stats_ = {};
    FrameContext ctx{
        .cmd = frame.command_buffer,
        .frame_index = frame_index_,
        .view = view,
        .targets = targets_,
        .output_extent = swapchain_.extent(),
        .stats = stats_,
        .timer = gpu_timer_,
    };
    RecordCommands(ctx, ui, output);
    if (ctx.drawable != nil) {
      [ctx.cmd presentDrawable:ctx.drawable];
    }
    context_.Commit(ctx.cmd);

    frame_index_ = (frame_index_ + 1) % kFramesInFlight;
  }
}

void RendererImpl::RecordCommands(FrameContext& ctx, ImDrawData* ui,
                                  Output output) {
  // Times `record` as `scope`.
  const auto timed = [&](GpuScope scope, const auto& record) {
    gpu_timer_.BeginScope(scope);
    record();
    gpu_timer_.EndScope(scope);
  };

  // TODO(metal-stage4): replaced by the scene pass; the test pattern shows
  // the orientation and colour handling of the post chain.
  timed(GpuScope::kScene, [&] {
    debug_gradient_pass_.Record(ctx, {.texture = ctx.targets.hdr().texture(),
                                      .extent = ctx.targets.extent()});
  });
  if (ctx.view.post.bloom_enabled) {
    timed(GpuScope::kBloom, [&] { bloom_pass_.Record(ctx); });
  }
  timed(GpuScope::kTonemap, [&] { tonemap_pass_.Record(ctx); });
  // TODO(metal-stage5): the overlay pass (debug lines, selection outline).

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

MeshId Renderer::UploadMesh(const asset::MeshData& data) {
  return impl_->UploadMesh(data);
}

void Renderer::ReleaseMesh(MeshId id) { impl_->ReleaseMesh(id); }

void Renderer::ReleaseTextures(std::span<const TextureId> ids) {
  impl_->ReleaseTextures(ids);
}

void Renderer::ReleaseMaterial(MaterialId id) { impl_->materials_.Release(id); }

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

// TODO(metal-stage4): the environment (sky and image-based lighting); until
// then these keep their signatures and do nothing.
void Renderer::SetEnvironment(const asset::ImageDataF& /*equirect*/,
                              float /*intensity*/, float /*rotation_radians*/) {
}

void Renderer::ResetEnvironment() {}

void Renderer::SetEnvironmentParams(float /*intensity*/,
                                    float /*rotation_radians*/) {}

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
