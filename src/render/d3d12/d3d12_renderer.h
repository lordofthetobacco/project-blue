#ifndef BLUE_RENDER_D3D12_D3D12_RENDERER_H_
#define BLUE_RENDER_D3D12_D3D12_RENDERER_H_

#include <d3d12.h>
#include <imgui.h>
#include <imgui_impl_dx12.h>
#include <wrl/client.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "asset/image_decoder.h"
#include "asset/mesh_data.h"
#include "camera/frustum.h"
#include "core/extent.h"
#include "material/material.h"
#include "platform/window.h"
#include "render/backend.h"
#include "render/draw_list.h"
#include "render/d3d12/buffer.h"
#include "render/d3d12/d3d12_context.h"
#include "render/d3d12/draw_record.h"
#include "render/d3d12/environment.h"
#include "render/d3d12/frame_globals.h"
#include "render/d3d12/fullscreen_pass.h"
#include "render/d3d12/gpu_timer.h"
#include "render/d3d12/image.h"
#include "render/d3d12/material_system.h"
#include "render/d3d12/mesh.h"
#include "render/d3d12/passes/bloom_pass.h"
#include "render/d3d12/passes/depth_prepass.h"
#include "render/d3d12/passes/mesh_pass.h"
#include "render/d3d12/passes/shadow_pass.h"
#include "render/d3d12/passes/skybox_pass.h"
#include "render/d3d12/passes/ssao_pass.h"
#include "render/d3d12/passes/tonemap_pass.h"
#include "render/d3d12/render_targets.h"
#include "render/d3d12/root_signature.h"
#include "render/d3d12/samplers.h"
#include "render/d3d12/shader_programs.h"
#include "render/d3d12/swapchain.h"
#include "render/d3d12/texture_cache.h"
#include "render/d3d12/upload_ring.h"
#include "render/frame_config.h"
#include "render/frame_plan.h"
#include "render/gpu_scope.h"
#include "render/memory_stats.h"
#include "render/renderer.h"
#include "render/renderer_backend.h"
#include "render/scene_view.h"
#include "render/texture_types.h"

// How a frame is built
// --------------------
// D3d12Renderer::DrawFrameTo() records the passes in order, each wrapped in
// a GPU timer scope, the same frame as the Vulkan backend's (passes/ holds
// one class or pair of functions per pass):
//
//   shadow cascades      ShadowPass        (sun depth, 4 layers)
//   depth pre-pass       DepthPrepass      draw list -> depth
//   ssao                 SsaoPass          depth -> ssao (half res, blurred)
//   scene (HDR)          BeginScenePass -> MeshPass (depth EQUAL),
//                        SkyboxPass -> EndScenePass
//   bloom                BloomPass         hdr -> bloom mip chain
//   tonemap              TonemapPass       hdr + bloom -> ldr (sRGB + luma)
//   present              FullscreenPass(present): ldr -> the window output
//                        (FXAA), or -> the editor viewport image
//   ui                   ImGui (imgui_impl_dx12) on the window output; the
//                        editor's window output is cleared to black first
//   readbacks            copies for RequestViewportReadback() /
//                        RequestOutputReadback()
//
// Not yet (package D4): the scope picture, the sights, weapon motion blur
// and the editor overlay; frames that ask for them render without them.
//
// The visible submeshes are culled and sorted once per frame by the shared
// DrawList and recorded by RecordDrawList() (draw_record.h). The frame set
// (FrameUniforms, shadow map, environment, BRDF LUT, AO) is FrameGlobals';
// the image-based lighting is Environment's.
//
// Headless (RendererOptions::window null) there is no swapchain: an
// offscreen R8G8B8A8_UNORM_SRGB output image of RendererOptions::
// headless_extent stands in for the swapchain buffer.
//
// States between frames: hdr, ldr, the environment images and the viewport
// image are PIXEL_SHADER_RESOURCE, the depth target DEPTH_READ |
// PIXEL_SHADER_RESOURCE, uploaded textures PIXEL | NON_PIXEL_SHADER_RESOURCE,
// the swapchain buffers PRESENT; every pass transitions what it uses through
// Image::Transition() (exact per-resource tracking, design §4.1).

namespace blue::render::d3d12 {

// The Direct3D 12 backend of render::Renderer. Every public function has the
// contract of the Renderer function of the same name (render/renderer.h);
// the effects not drawn yet are listed above. Created by
// CreateD3d12Renderer(). Neither copyable nor movable. Not thread-safe.
class D3d12Renderer final : public RendererBackend {
 public:
  // Takes `context`, which must have a device (D3d12Context::has_device()).
  D3d12Renderer(const RendererOptions& options,
                std::unique_ptr<D3d12Context> context);
  ~D3d12Renderer() override;

  [[nodiscard]] Backend backend() const override { return Backend::kD3d12; }
  [[nodiscard]] BackendCaps caps() const override { return {}; }

  void DrawFrame(const SceneView& view, ImDrawData* ui) override;
  [[nodiscard]] ViewportImage PrepareViewport(core::Extent2D size) override;
  [[nodiscard]] const ViewportImage& viewport() const override {
    return viewport_;
  }
  void DrawViewportFrame(const SceneView& view, ImDrawData* ui) override;
  [[nodiscard]] uint64_t RequestViewportReadback() override;
  [[nodiscard]] uint64_t RequestOutputReadback() override;
  [[nodiscard]] ReadbackStatus TakeViewportReadback(
      uint64_t ticket, ViewportPixels& pixels) override;

  [[nodiscard]] MeshId UploadMesh(
      std::span<const asset::Vertex> vertices,
      std::span<const asset::Index> indices) override;
  void ReleaseMesh(MeshId id) override;
  void ReleaseTextures(std::span<const TextureId> ids) override;
  void ReleaseMaterial(MaterialId id) override { materials_.Release(id); }
  void FlushReleases() override;
  [[nodiscard]] ImTextureID ui_texture(TextureId id) const override {
    return static_cast<ImTextureID>(textures_.image(id).shader_srv().ptr);
  }

  [[nodiscard]] MaterialId CreateMaterial(
      const material::MaterialDesc& desc) override {
    return materials_.Create(desc);
  }
  [[nodiscard]] std::vector<TextureId> UploadTextures(
      std::span<const TextureUpload> textures) override {
    return textures_.Upload(textures);
  }
  [[nodiscard]] MaterialId CreateMaterial(
      const material::MaterialDesc& desc,
      const MaterialTextures& textures) override {
    return materials_.Create(desc, textures);
  }
  void UpdateMaterial(MaterialId id,
                      const material::MaterialDesc& desc) override {
    materials_.Update(id, desc);
  }
  [[nodiscard]] const material::MaterialDesc& material(
      MaterialId id) const override {
    return materials_.desc(id);
  }
  [[nodiscard]] size_t material_count() const override {
    return materials_.count();
  }
  [[nodiscard]] TextureStats texture_stats() const override {
    return textures_.stats();
  }
  [[nodiscard]] MemoryStats memory_stats() const override;

  void SetEnvironment(const asset::ImageDataF& equirect, float intensity,
                      float rotation_radians) override {
    context_.WaitIdle();
    environment_.Set(equirect, intensity, rotation_radians);
  }
  void ResetEnvironment() override {
    context_.WaitIdle();
    environment_.Set({}, 1.0f, 0.0f);
  }
  void SetEnvironmentParams(float intensity, float rotation_radians) override {
    environment_.SetParams(intensity, rotation_radians);
  }

  void RequestSwapchainRebuild() override { swapchain_dirty_ = true; }
  void SetVsync(bool enabled) override { vsync_ = enabled; }
  void ReloadChangedShaders() override { shader_programs_.ReloadChanged(); }
  [[nodiscard]] std::string_view last_shader_error() const override {
    return shader_programs_.last_error();
  }

  [[nodiscard]] bool vsync() const override { return vsync_; }
  [[nodiscard]] std::string_view device_name() const override {
    return context_.device_name();
  }
  [[nodiscard]] core::Extent2D output_extent() const override;
  // No temporal scaler: the scene is rendered at the size of its target
  // (the window or the editor viewport).
  [[nodiscard]] core::Extent2D render_extent() const override {
    return targets_.extent();
  }
  [[nodiscard]] core::Extent2D scene_output_extent() const override {
    return render_extent();
  }
  [[nodiscard]] Upscaling active_upscaling() const override {
    return Upscaling::kOff;
  }
  void WaitIdle() const override { context_.WaitIdle(); }

  [[nodiscard]] std::optional<double> gpu_frame_ms() const override {
    return gpu_timer_.last_frame_ms();
  }
  [[nodiscard]] std::optional<double> gpu_scope_ms(
      GpuScope scope) const override {
    return gpu_timer_.last_ms(scope);
  }
  [[nodiscard]] RenderStats stats() const override { return stats_; }
  [[nodiscard]] ValidationStats validation_stats() const override {
    const D3d12Context::ValidationCounts counts = context_.validation_counts();
    return {.enabled = context_.validation(),
            .errors = counts.errors,
            .warnings = counts.warnings};
  }

  void InitImGuiRenderer() const override;
  void ShutdownImGuiRenderer() const override;

  // Direct3D 12-only access for the backend's tests.
  [[nodiscard]] const D3d12Context& context() const { return context_; }
  [[nodiscard]] const TextureCache& textures() const { return textures_; }
  [[nodiscard]] const Samplers& samplers() const { return samplers_; }
  [[nodiscard]] const RootSignature& fullscreen_root_signature() const {
    return fullscreen_root_;
  }
  [[nodiscard]] bool headless() const { return !swapchain_.has_value(); }

 private:
  // Per-frame-in-flight command recording.
  struct Frame {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    uint64_t fence_value = 0;  // Reached: the frame's work has finished.
  };

  // Where a frame's scene goes.
  enum class Output {
    kWindow,    // The window output (DrawFrame()).
    kViewport,  // viewport_image_, with the UI alone on the window output.
  };

  // What a readback copies.
  enum class ReadbackSource : uint8_t {
    kViewport,  // viewport_image_ (RequestViewportReadback()).
    kOutput,    // The window output (RequestOutputReadback()).
  };

  // Requested copies.
  struct Readback {
    uint64_t ticket = 0;
    ReadbackSource source = ReadbackSource::kViewport;
    // The frame that records the copy (0 until recorded) and its fence value
    // (0 until submitted).
    uint64_t frame_serial = 0;
    uint64_t fence_value = 0;
    core::Extent2D extent{};
    uint32_t row_pitch = 0;
    Buffer buffer;
  };

  template <typename T>
  struct Retired {
    T object;
    uint64_t last_frame = 0;  // The last frame serial that may use it.
  };

  void CreateFrames();
  // Destroys the retired meshes and images no frame in flight can use any
  // more. Call after the current frame's fence was reached.
  void DestroyRetired();
  // Destroys every retired object if no frame is in flight (does not wait).
  void DestroyRetiredIfIdle();
  // Blocks until no frame in flight runs on the GPU any more.
  void WaitForFramesInFlight() const;
  // Points FrameGlobals at the current shadow map and AO target; after the
  // targets were (re)created.
  void BindTargets();
  // Computes the world-space bounds of every draw item into world_bounds_.
  void ComputeWorldBounds(const SceneView& view);
  void ResizeViewport(core::Extent2D size);
  [[nodiscard]] bool RebuildSwapchain();
  // The body of DrawFrame() and DrawViewportFrame().
  void DrawFrameTo(const SceneView& view, ImDrawData* ui, Output output);
  // Records the window output's UI (and, for kViewport, its clear).
  void RecordUi(ID3D12GraphicsCommandList* cmd, ImDrawData* ui,
                Output output, D3D12_CPU_DESCRIPTOR_HANDLE target);
  // Records the copy of `resource` (in COPY_SOURCE) for every readback of
  // `source` not recorded yet.
  void RecordReadbacks(ID3D12GraphicsCommandList* cmd, ReadbackSource source,
                       ID3D12Resource* resource, core::Extent2D extent);
  [[nodiscard]] bool HasPendingReadback(ReadbackSource source) const;

  // ImGui's descriptor callbacks (ImGui_ImplDX12_InitInfo::UserData is the
  // renderer).
  static void AllocateImGuiDescriptor(ImGui_ImplDX12_InitInfo* info,
                                      D3D12_CPU_DESCRIPTOR_HANDLE* cpu,
                                      D3D12_GPU_DESCRIPTOR_HANDLE* gpu);
  static void FreeImGuiDescriptor(ImGui_ImplDX12_InitInfo* info,
                                  D3D12_CPU_DESCRIPTOR_HANDLE cpu,
                                  D3D12_GPU_DESCRIPTOR_HANDLE gpu);

  // Declared first: everything below uses the context.
  std::unique_ptr<D3d12Context> context_owner_;
  const D3d12Context& context_;
  const platform::Window* window_;  // Null when headless.
  core::Extent2D headless_extent_;
  bool vsync_ = true;
  bool swapchain_dirty_ = false;
  std::optional<Swapchain> swapchain_;
  Samplers samplers_;
  UploadRing upload_ring_;
  TextureCache textures_;
  MaterialSystem materials_;
  ShaderPrograms shader_programs_;
  RootSignature fullscreen_root_;
  // The scene families (frame set + material / sky / scope set); the scope
  // one is used from package D4 on.
  RootSignature scene_material_root_;
  RootSignature scene_sky_root_;
  RootSignature scene_scope_root_;
  RenderTargets targets_;
  FrameGlobals frame_globals_;
  Environment environment_;
  // Declared before the passes that borrow it.
  std::vector<Mesh> meshes_;
  // The meshes and materials as the draw list reads them.
  DrawListSource draw_source_{materials_, meshes_};
  DrawList draw_list_;
  // World-space bounds of the current frame's draw items (parallel to
  // SceneView::draws); a member to reuse its allocation.
  std::vector<camera::Aabb> world_bounds_;
  ShadowPass shadow_pass_;
  DepthPrepass depth_prepass_;
  SsaoPass ssao_pass_;
  MeshPass mesh_pass_;
  SkyboxPass skybox_pass_;
  BloomPass bloom_pass_;
  TonemapPass tonemap_pass_;
  FullscreenPass present_pass_;
  // The present pass into the editor viewport image; created by the first
  // PrepareViewport().
  std::optional<FullscreenPass> viewport_present_pass_;

  uint64_t mesh_bytes_ = 0;
  uint32_t mesh_count_ = 0;
  std::vector<Retired<Mesh>> retired_meshes_;
  std::vector<Retired<Image>> retired_images_;

  // The window output when headless.
  Image output_image_;
  // Editor viewport output (PrepareViewport()); empty in the game.
  Image viewport_image_;
  mutable ViewportImage viewport_;
  uint64_t viewport_resized_at_ = 0;
  // Whether imgui_impl_dx12 is initialized (InitImGuiRenderer()).
  mutable bool imgui_initialized_ = false;

  std::vector<Readback> readbacks_;
  uint64_t next_readback_ticket_ = 1;

  std::array<Frame, kFramesInFlight> frames_;
  size_t frame_index_ = 0;
  // Number of the frame being recorded; counts every recorded frame from 1.
  uint64_t frame_serial_ = 0;
  RenderStats stats_;
  GpuTimer gpu_timer_;
};

}  // namespace blue::render::d3d12

namespace blue::render {

// Returns the Direct3D 12 renderer for `options`, or null when no adapter
// qualifies (CreateBackend() then tries another backend).
[[nodiscard]] std::unique_ptr<RendererBackend> CreateD3d12Renderer(
    const RendererOptions& options);

}  // namespace blue::render

#endif  // BLUE_RENDER_D3D12_D3D12_RENDERER_H_
