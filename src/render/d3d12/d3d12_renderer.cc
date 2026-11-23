#include "render/d3d12/d3d12_renderer.h"

#include <D3D12MemAlloc.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <imgui.h>
#include <imgui_impl_dx12.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "camera/frustum.h"
#include "core/extent.h"
#include "lighting/cascades.h"
#include "render/frame_plan.h"
#include "render/frame_uniforms.h"
#include "core/log.h"
#include "core/path.h"
#include "platform/window.h"
#include "render/binding_layout.h"
#include "render/d3d12/buffer.h"
#include "render/d3d12/d3d12_check.h"
#include "render/d3d12/d3d12_context.h"
#include "render/d3d12/format.h"
#include "render/d3d12/image.h"
#include "render/d3d12/passes/frame_context.h"
#include "render/d3d12/passes/scene_pass.h"
#include "render/d3d12/root_layout.h"
#include "render/d3d12/staging_upload.h"
#include "render/frame_config.h"
#include "render/gpu_scope.h"
#include "render/memory_stats.h"
#include "render/renderer.h"
#include "render/scene_view.h"
#include "render/shader_manifest.h"

namespace blue::render::d3d12 {

namespace {

// The window output's format when headless and the editor viewport's
// (resources and views): what the swapchain's views are.
constexpr DXGI_FORMAT kOutputFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
// The editor's window output under the UI.
constexpr float kUiBackground[4] = {0.0f, 0.0f, 0.0f, 1.0f};

// Neutral grey dielectric: what kDefaultMaterial looks like.
material::MaterialDesc DefaultMaterialDesc() {
  material::MaterialDesc desc;
  desc.name = "default";
  desc.base_color = glm::vec4(0.5f, 0.5f, 0.5f, 1.0f);
  desc.roughness = 0.6f;
  return desc;
}

// Must match PresentConstants in shaders/present.slang.
struct PresentConstants {
  int32_t decode_srgb = 0;
  int32_t fxaa = 0;
  float texel_size[2] = {0.0f, 0.0f};
};

std::optional<Swapchain> MakeSwapchain(const D3d12Context& context,
                                       const platform::Window* window) {
  if (window == nullptr) {
    return std::nullopt;
  }
  return std::optional<Swapchain>(std::in_place, context,
                                  static_cast<HWND>(window->native_handle()),
                                  window->PixelSize());
}

}  // namespace

D3d12Renderer::D3d12Renderer(const RendererOptions& options,
                             std::unique_ptr<D3d12Context> context)
    : context_owner_(std::move(context)),
      context_(*context_owner_),
      window_(options.window),
      headless_extent_{std::max(options.headless_extent.width, 1u),
                       std::max(options.headless_extent.height, 1u)},
      swapchain_(MakeSwapchain(context_, options.window)),
      samplers_(context_, options.max_anisotropy),
      upload_ring_(context_),
      textures_(context_),
      materials_(context_, textures_, samplers_),
      shader_programs_(context_, core::JoinPath(options.shader_dir, "dxil"),
                       options.shader_source_dir, options.shader_mode),
      fullscreen_root_(context_.device(), FullscreenRootLayout(),
                       L"blue fullscreen root signature"),
      scene_material_root_(context_.device(),
                           SceneRootLayout(kMaterialBindings),
                           L"blue scene material root signature"),
      scene_sky_root_(context_.device(), SceneRootLayout(kSkyBindings),
                      L"blue scene sky root signature"),
      scene_scope_root_(context_.device(), SceneRootLayout(kScopeBindings),
                        L"blue scene scope root signature"),
      targets_(context_, output_extent()),
      frame_globals_(context_, samplers_),
      environment_(context_, shader_programs_, fullscreen_root_, samplers_,
                   frame_globals_),
      shadow_pass_(context_, shader_programs_, scene_material_root_, meshes_),
      depth_prepass_(context_, shader_programs_, scene_material_root_,
                     frame_globals_, materials_, meshes_),
      ssao_pass_(context_, shader_programs_, fullscreen_root_, samplers_),
      mesh_pass_(context_, shader_programs_, scene_material_root_,
                 frame_globals_, materials_, meshes_),
      skybox_pass_(context_, shader_programs_, scene_sky_root_, frame_globals_,
                   environment_, samplers_),
      bloom_pass_(context_, shader_programs_, fullscreen_root_, samplers_),
      tonemap_pass_(context_, shader_programs_, fullscreen_root_, samplers_),
      present_pass_(context_, shader_programs_, fullscreen_root_,
                    ShaderProgram::kPresent, kOutputFormat),
      gpu_timer_(context_) {
  if (!swapchain_) {
    output_image_ = MakeColorTarget(context_, headless_extent_, kOutputFormat,
                               D3D12_RESOURCE_STATE_COPY_SOURCE, kUiBackground,
                               false, L"blue headless output");
    core::log::Info("Headless output {} x {}", headless_extent_.width,
                    headless_extent_.height);
  }
  BindTargets();
  const MaterialId default_material = materials_.Create(DefaultMaterialDesc());
  if (default_material != kDefaultMaterial) {
    core::log::Fatal("The default material must be created first");
  }
  CreateFrames();
}

D3d12Renderer::~D3d12Renderer() {
  context_.WaitIdle();
  // ImGuiLayer shuts the backend down before; this covers a renderer
  // destroyed without it.
  ShutdownImGuiRenderer();
}

void D3d12Renderer::CreateFrames() {
  ID3D12Device* device = context_.device();
  for (Frame& frame : frames_) {
    CheckHr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                           IID_PPV_ARGS(&frame.allocator)));
    CheckHr(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                      frame.allocator.Get(), nullptr,
                                      IID_PPV_ARGS(&frame.list)));
    frame.list->SetName(L"blue frame commands");
    CheckHr(frame.list->Close());
  }
}

MeshId D3d12Renderer::UploadMesh(std::span<const asset::Vertex> vertices,
                                 std::span<const asset::Index> indices) {
  const Mesh& mesh = meshes_.emplace_back(context_, vertices, indices);
  mesh_bytes_ += mesh.size_bytes();
  mesh_count_ += 1;
  return static_cast<MeshId>(meshes_.size() - 1);
}

void D3d12Renderer::ReleaseMesh(MeshId id) {
  const auto index = static_cast<size_t>(id);
  if (index >= meshes_.size() || !meshes_[index].resident()) {
    return;
  }
  mesh_bytes_ -= meshes_[index].size_bytes();
  mesh_count_ -= 1;
  // The moved-from mesh left in the slot is the tombstone.
  retired_meshes_.push_back(
      {.object = std::move(meshes_[index]), .last_frame = frame_serial_});
}

void D3d12Renderer::ReleaseTextures(std::span<const TextureId> ids) {
  for (const TextureId id : ids) {
    Image image = textures_.TakeUploaded(id);
    if (image.valid()) {
      retired_images_.push_back(
          {.object = std::move(image), .last_frame = frame_serial_});
    }
  }
}

void D3d12Renderer::DestroyRetired() {
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

void D3d12Renderer::DestroyRetiredIfIdle() {
  if (retired_meshes_.empty() && retired_images_.empty()) {
    return;
  }
  for (const Frame& frame : frames_) {
    if (!context_.Reached(frame.fence_value)) {
      return;
    }
  }
  retired_meshes_.clear();
  retired_images_.clear();
}

void D3d12Renderer::FlushReleases() {
  if (retired_meshes_.empty() && retired_images_.empty()) {
    return;
  }
  WaitForFramesInFlight();
  retired_meshes_.clear();
  retired_images_.clear();
}

void D3d12Renderer::WaitForFramesInFlight() const {
  for (const Frame& frame : frames_) {
    context_.WaitFor(frame.fence_value);
  }
}

MemoryStats D3d12Renderer::memory_stats() const {
  MemoryStats stats;
  D3D12MA::Budget local{};
  D3D12MA::Budget non_local{};
  context_.allocator()->GetBudget(&local, &non_local);
  DXGI_ADAPTER_DESC1 adapter{};
  context_.adapter()->GetDesc1(&adapter);
  const auto heap = [](const D3D12MA::Budget& budget, uint64_t size,
                       bool device_local) {
    return HeapMemoryStats{
        .heap_size = size,
        .device_local = device_local,
        .block_bytes = budget.Stats.BlockBytes,
        .allocation_bytes = budget.Stats.AllocationBytes,
        .block_count = budget.Stats.BlockCount,
        .allocation_count = budget.Stats.AllocationCount,
        .usage = budget.UsageBytes,
        .budget = budget.BudgetBytes,
    };
  };
  stats.heaps[0] = heap(local, adapter.DedicatedVideoMemory, true);
  stats.heaps[1] = heap(non_local, adapter.SharedSystemMemory, false);
  stats.heap_count = 2;
  stats.driver_budget = true;

  const TextureStats textures = textures_.stats();
  stats.texture_bytes = textures.bytes;
  stats.texture_count = static_cast<uint32_t>(textures.count);
  stats.mesh_bytes = mesh_bytes_;
  stats.mesh_count = mesh_count_;
  stats.render_target_bytes = targets_.size_bytes() +
                              shadow_pass_.map().size_bytes() +
                              viewport_image_.size_bytes() +
                              output_image_.size_bytes();
  stats.environment_bytes = environment_.size_bytes();
  for (const Retired<Mesh>& retired : retired_meshes_) {
    stats.retired_bytes += retired.object.size_bytes();
  }
  for (const Retired<Image>& retired : retired_images_) {
    stats.retired_bytes += retired.object.size_bytes();
  }
  const D3d12Context::StagingUsage staging = context_.staging_usage();
  stats.staging_budget_bytes = 2 * StagingUploader::kChunkBytes;
  stats.staging_peak_bytes = staging.peak;
  stats.staging_current_bytes = staging.current;
  return stats;
}

core::Extent2D D3d12Renderer::output_extent() const {
  return swapchain_ ? swapchain_->extent() : headless_extent_;
}

void D3d12Renderer::BindTargets() {
  frame_globals_.BindShadowMap(shadow_pass_.map());
  frame_globals_.BindAmbientOcclusion(targets_.ssao());
  ssao_pass_.TargetsRecreated();
}

void D3d12Renderer::ComputeWorldBounds(const SceneView& view) {
  world_bounds_.clear();
  world_bounds_.reserve(view.draws.size());
  for (const DrawItem& item : view.draws) {
    world_bounds_.push_back(camera::TransformAabb(
        {.min = item.bounds.min, .max = item.bounds.max}, item.transform));
  }
}

bool D3d12Renderer::RebuildSwapchain() {
  const core::Extent2D size = window_->PixelSize();
  if (size.width == 0 || size.height == 0) {
    return false;  // No area; stay dirty and retry next frame.
  }
  context_.WaitIdle();
  swapchain_->Resize(size);
  if (viewport_.generation == 0) {  // Otherwise they follow the viewport.
    targets_.Resize(swapchain_->extent());
    BindTargets();
  }
  swapchain_dirty_ = false;
  return true;
}

ViewportImage D3d12Renderer::PrepareViewport(core::Extent2D requested) {
  const core::Extent2D size{std::max(requested.width, 1u),
                            std::max(requested.height, 1u)};
  if (!viewport_present_pass_) {
    viewport_present_pass_.emplace(context_, shader_programs_,
                                   fullscreen_root_, ShaderProgram::kPresent,
                                   kOutputFormat);
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

void D3d12Renderer::ResizeViewport(core::Extent2D size) {
  WaitForFramesInFlight();
  // The UI may sample it before the first DrawViewportFrame(): created in
  // PIXEL_SHADER_RESOURCE.
  viewport_image_ =
      MakeColorTarget(context_, size, kOutputFormat,
                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, kUiBackground,
                 /*shader_visible=*/true, L"blue viewport output");
  targets_.Resize(size);
  BindTargets();
  viewport_ = {
      .texture = static_cast<ImTextureID>(viewport_image_.shader_srv().ptr),
      .extent = size,
      .generation = viewport_.generation + 1};
  viewport_resized_at_ = frame_serial_;
  core::log::Info("Viewport output {} x {}", size.width, size.height);
}

void D3d12Renderer::DrawFrame(const SceneView& view, ImDrawData* ui) {
  DrawFrameTo(view, ui, Output::kWindow);
}

void D3d12Renderer::DrawViewportFrame(const SceneView& view, ImDrawData* ui) {
  if (viewport_.generation == 0) {
    core::log::Fatal("DrawViewportFrame() before PrepareViewport()");
  }
  DrawFrameTo(view, ui, Output::kViewport);
}

uint64_t D3d12Renderer::RequestViewportReadback() {
  const uint64_t ticket = next_readback_ticket_++;
  readbacks_.push_back({.ticket = ticket, .source = ReadbackSource::kViewport});
  return ticket;
}

uint64_t D3d12Renderer::RequestOutputReadback() {
  const uint64_t ticket = next_readback_ticket_++;
  readbacks_.push_back({.ticket = ticket, .source = ReadbackSource::kOutput});
  return ticket;
}

ReadbackStatus D3d12Renderer::TakeViewportReadback(uint64_t ticket,
                                                   ViewportPixels& pixels) {
  const auto it = std::ranges::find(readbacks_, ticket, &Readback::ticket);
  if (it == readbacks_.end()) {
    return ReadbackStatus::kUnknown;
  }
  if (it->fence_value == 0 || !context_.Reached(it->fence_value)) {
    return ReadbackStatus::kPending;
  }
  const auto* source = static_cast<const std::byte*>(it->buffer.mapped());
  const size_t row_bytes = size_t{it->extent.width} * 4;
  pixels.width = it->extent.width;
  pixels.height = it->extent.height;
  pixels.rgba.resize(row_bytes * it->extent.height);
  for (uint32_t row = 0; row < it->extent.height; ++row) {
    std::memcpy(pixels.rgba.data() + row * row_bytes,
                source + size_t{row} * it->row_pitch, row_bytes);
  }
  readbacks_.erase(it);
  return ReadbackStatus::kReady;
}

bool D3d12Renderer::HasPendingReadback(ReadbackSource source) const {
  return std::ranges::any_of(readbacks_, [source](const Readback& readback) {
    return readback.frame_serial == 0 && readback.source == source;
  });
}

void D3d12Renderer::RecordReadbacks(ID3D12GraphicsCommandList* cmd,
                                    ReadbackSource source,
                                    ID3D12Resource* resource,
                                    core::Extent2D extent) {
  for (Readback& readback : readbacks_) {
    if (readback.frame_serial != 0 || readback.source != source) {
      continue;
    }
    readback.frame_serial = frame_serial_;
    readback.extent = extent;
    readback.row_pitch = static_cast<uint32_t>(
        AlignUp(uint64_t{extent.width} * 4, kRowPitchAlignment));
    readback.buffer =
        Buffer(context_, uint64_t{readback.row_pitch} * extent.height,
               Buffer::Access::kReadback, L"blue readback");
    const D3D12_TEXTURE_COPY_LOCATION destination{
        .pResource = readback.buffer.resource(),
        .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
        .PlacedFootprint = {.Offset = 0,
                            .Footprint = {.Format = DXGI_FORMAT_R8G8B8A8_UNORM,
                                          .Width = extent.width,
                                          .Height = extent.height,
                                          .Depth = 1,
                                          .RowPitch = readback.row_pitch}},
    };
    const D3D12_TEXTURE_COPY_LOCATION copy_source{
        .pResource = resource,
        .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
        .SubresourceIndex = 0,
    };
    cmd->CopyTextureRegion(&destination, 0, 0, 0, &copy_source, nullptr);
  }
}

void D3d12Renderer::RecordUi(ID3D12GraphicsCommandList* cmd, ImDrawData* ui,
                             Output output,
                             D3D12_CPU_DESCRIPTOR_HANDLE target) {
  if (output == Output::kViewport) {
    cmd->ClearRenderTargetView(target, kUiBackground, 0, nullptr);
  }
  if (ui == nullptr || !imgui_initialized_) {
    return;
  }
  cmd->OMSetRenderTargets(1, &target, FALSE, nullptr);
  ImGui_ImplDX12_NewFrame();
  ImGui_ImplDX12_RenderDrawData(ui, cmd);
  // ImGui may bind its own heaps: later commands get the backend's back.
  context_.descriptors().Bind(cmd);
}

void D3d12Renderer::DrawFrameTo(const SceneView& view, ImDrawData* ui,
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
    swapchain_->WaitForFrame();
  }

  Frame& frame = frames_[frame_index_];
  context_.WaitFor(frame.fence_value);
  gpu_timer_.Collect(frame_index_);
  ++frame_serial_;
  DescriptorHeaps& heaps = context_.descriptors();
  heaps.BeginFrame(frame_serial_);
  upload_ring_.BeginFrame(frame_serial_);
  materials_.BeginFrame(frame_serial_);
  DestroyRetired();

  // Cull and sort; the cascades; the frame's uniforms.
  const FramePlan plan = MakeFramePlan(view, caps());
  ComputeWorldBounds(view);
  const camera::Frustum camera_frustum(view.camera.projection *
                                       view.camera.view);
  stats_ = {};
  draw_list_.Build(view, world_bounds_, camera_frustum,
                   draw_source_.resources(), stats_);
  std::array<lighting::Cascade, lighting::kCascadeCount> cascades{};
  const core::Extent2D scene_extent = targets_.extent();
  if (plan.shadows) {
    const float aspect = static_cast<float>(scene_extent.width) /
                         static_cast<float>(std::max(scene_extent.height, 1u));
    cascades = lighting::FitCascades(
        view.camera.view, view.camera.vertical_fov_radians, aspect,
        view.camera.near_plane, view.sun.direction);
  }
  FrameUniforms uniforms =
      MakeFrameUniforms(view, cascades, plan.shadows, scene_extent);
  environment_.FillUniforms(uniforms);

  CheckHr(frame.allocator->Reset());
  ID3D12GraphicsCommandList* cmd = frame.list.Get();
  CheckHr(cmd->Reset(frame.allocator.Get(), nullptr));
  heaps.Bind(cmd);
  gpu_timer_.Begin(cmd, frame_index_, GpuScope::kTotal);

  const FrameContext ctx{
      .cmd = cmd,
      .view = view,
      .plan = plan,
      .targets = targets_,
      .frame = frame_globals_.Prepare(uniforms, upload_ring_),
      .draw_list = draw_list_.entries(),
      .world_bounds = world_bounds_,
      .cascades = cascades,
      .stats = stats_,
  };
  // Times `record` as `scope`.
  const auto timed = [&](GpuScope scope, const auto& record) {
    gpu_timer_.Begin(cmd, frame_index_, scope);
    record();
    gpu_timer_.End(cmd, frame_index_, scope);
  };
  if (plan.shadows) {
    timed(GpuScope::kShadow, [&] { shadow_pass_.Record(ctx); });
  }
  timed(GpuScope::kDepthPrepass, [&] { depth_prepass_.Record(ctx); });
  if (plan.ssao) {
    timed(GpuScope::kSsao, [&] { ssao_pass_.Record(ctx); });
  } else {
    ssao_pass_.Record(ctx);  // At most a one-time clear to white.
  }
  timed(GpuScope::kScene, [&] {
    BeginScenePass(ctx);
    mesh_pass_.Draw(ctx);
    skybox_pass_.Draw(ctx);
    EndScenePass(ctx);
  });
  if (plan.bloom) {
    timed(GpuScope::kBloom, [&] { bloom_pass_.Record(ctx); });
  }
  timed(GpuScope::kTonemap, [&] { tonemap_pass_.Record(ctx); });

  // The window output: the swapchain buffer, or the headless output image.
  ID3D12Resource* window_resource = nullptr;
  D3D12_CPU_DESCRIPTOR_HANDLE window_rtv{};
  {
    BarrierBatch barriers(cmd);
    if (swapchain_) {
      swapchain_->Transition(barriers, D3D12_RESOURCE_STATE_RENDER_TARGET);
      window_resource = swapchain_->buffer();
      window_rtv = swapchain_->rtv();
    } else {
      output_image_.Transition(barriers, D3D12_RESOURCE_STATE_RENDER_TARGET);
      window_resource = output_image_.resource();
      window_rtv = output_image_.rtv();
    }
    if (output == Output::kViewport) {
      viewport_image_.Transition(barriers, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
  }
  const core::Extent2D window_extent = output_extent();

  // Present: LDR -> the frame's scene output.
  gpu_timer_.Begin(cmd, frame_index_, GpuScope::kPresent);
  {
    const PresentConstants constants{
        .decode_srgb = 1,  // Every output has _SRGB views.
        .fxaa = view.post.fxaa_enabled ? 1 : 0,
        .texel_size = {1.0f / static_cast<float>(scene_extent.width),
                       1.0f / static_cast<float>(scene_extent.height)},
    };
    const std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 1> inputs = {
        targets_.ldr().staging_srv()};
    if (output == Output::kViewport) {
      viewport_present_pass_->Record(cmd, viewport_image_.rtv(), scene_extent,
                                     inputs, samplers_.linear_clamp(),
                                     std::as_bytes(std::span(&constants, 1)));
    } else {
      present_pass_.Record(cmd, window_rtv, window_extent, inputs,
                           samplers_.linear_clamp(),
                           std::as_bytes(std::span(&constants, 1)));
    }
  }
  gpu_timer_.End(cmd, frame_index_, GpuScope::kPresent);

  if (output == Output::kViewport) {
    if (HasPendingReadback(ReadbackSource::kViewport)) {
      {
        BarrierBatch barriers(cmd);
        viewport_image_.Transition(barriers, D3D12_RESOURCE_STATE_COPY_SOURCE);
      }
      RecordReadbacks(cmd, ReadbackSource::kViewport,
                      viewport_image_.resource(), viewport_image_.extent());
    }
    BarrierBatch barriers(cmd);
    viewport_image_.Transition(barriers,
                               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  }

  gpu_timer_.Begin(cmd, frame_index_, GpuScope::kUi);
  RecordUi(cmd, ui, output, window_rtv);
  gpu_timer_.End(cmd, frame_index_, GpuScope::kUi);

  {
    BarrierBatch barriers(cmd);
    const D3D12_RESOURCE_STATES copy = D3D12_RESOURCE_STATE_COPY_SOURCE;
    if (swapchain_) {
      swapchain_->Transition(barriers, copy);
    } else {
      output_image_.Transition(barriers, copy);
    }
  }
  RecordReadbacks(cmd, ReadbackSource::kOutput, window_resource,
                  window_extent);
  if (swapchain_) {
    BarrierBatch barriers(cmd);
    swapchain_->Transition(barriers, D3D12_RESOURCE_STATE_PRESENT);
  }

  gpu_timer_.End(cmd, frame_index_, GpuScope::kTotal);
  gpu_timer_.EndFrame(cmd, frame_index_);
  heaps.EndFrame(frame_serial_);
  upload_ring_.EndFrame(frame_serial_);
  CheckHr(cmd->Close());
  frame.fence_value = context_.Execute(cmd);
  for (Readback& readback : readbacks_) {
    if (readback.frame_serial == frame_serial_) {
      readback.fence_value = frame.fence_value;
    }
  }
  if (swapchain_) {
    swapchain_->Present(vsync_);
  }
  context_.PollValidationMessages();
  frame_index_ = (frame_index_ + 1) % kFramesInFlight;
}

void D3d12Renderer::AllocateImGuiDescriptor(ImGui_ImplDX12_InitInfo* info,
                                            D3D12_CPU_DESCRIPTOR_HANDLE* cpu,
                                            D3D12_GPU_DESCRIPTOR_HANDLE* gpu) {
  const auto* renderer = static_cast<const D3d12Renderer*>(info->UserData);
  const Descriptor descriptor = renderer->context_.descriptors().AllocateSingle();
  *cpu = descriptor.cpu;
  *gpu = descriptor.gpu;
}

void D3d12Renderer::FreeImGuiDescriptor(ImGui_ImplDX12_InitInfo* info,
                                        D3D12_CPU_DESCRIPTOR_HANDLE cpu,
                                        D3D12_GPU_DESCRIPTOR_HANDLE gpu) {
  static_cast<void>(gpu);
  const auto* renderer = static_cast<const D3d12Renderer*>(info->UserData);
  DescriptorHeaps& heaps = renderer->context_.descriptors();
  // Frames up to the current one may have drawn with it.
  heaps.RetireSingle(heaps.SingleAt(cpu), renderer->frame_serial_);
}

void D3d12Renderer::InitImGuiRenderer() const {
  ImGui_ImplDX12_InitInfo info;
  info.Device = context_.device();
  info.CommandQueue = context_.queue();
  info.NumFramesInFlight = static_cast<int>(kFramesInFlight);
  info.RTVFormat = kOutputFormat;
  info.DSVFormat = DXGI_FORMAT_UNKNOWN;
  info.UserData = const_cast<D3d12Renderer*>(this);
  info.SrvDescriptorHeap = context_.descriptors().shader_visible_heap();
  info.SrvDescriptorAllocFn = &AllocateImGuiDescriptor;
  info.SrvDescriptorFreeFn = &FreeImGuiDescriptor;
  if (!ImGui_ImplDX12_Init(&info)) {
    core::log::Fatal("ImGui_ImplDX12_Init failed");
  }
  imgui_initialized_ = true;
}

void D3d12Renderer::ShutdownImGuiRenderer() const {
  if (!imgui_initialized_) {
    return;
  }
  ImGui_ImplDX12_Shutdown();
  imgui_initialized_ = false;
}

}  // namespace blue::render::d3d12

namespace blue::render {

std::unique_ptr<RendererBackend> CreateD3d12Renderer(
    const RendererOptions& options) {
  // Debug builds validate whatever options.gpu_validation says.
  auto context = std::make_unique<d3d12::D3d12Context>(options.adapter,
                                                       options.gpu_validation);
  if (!context->has_device()) {
    return nullptr;
  }
  return std::make_unique<d3d12::D3d12Renderer>(options, std::move(context));
}

}  // namespace blue::render
