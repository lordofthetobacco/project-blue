#ifndef BLUE_RENDER_VULKAN_VULKAN_RENDERER_H_
#define BLUE_RENDER_VULKAN_VULKAN_RENDERER_H_

#include <imgui.h>
#include <vulkan/vulkan.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "asset/image_decoder.h"
#include "asset/mesh_data.h"
#include "camera/frustum.h"
#include "core/extent.h"
#include "lighting/cascades.h"
#include "material/material.h"
#include "platform/window.h"
#include "render/backend.h"
#include "render/draw_list.h"
#include "render/frame_config.h"
#include "render/frame_plan.h"
#include "render/gpu_scope.h"
#include "render/memory_stats.h"
#include "render/renderer.h"
#include "render/renderer_backend.h"
#include "render/scene_view.h"
#include "render/texture_types.h"
#include "render/vulkan/barriers.h"
#include "render/vulkan/buffer.h"
#include "render/vulkan/draw_record.h"
#include "render/vulkan/environment.h"
#include "render/vulkan/frame_globals.h"
#include "render/vulkan/gpu_timer.h"
#include "render/vulkan/image.h"
#include "render/vulkan/material_system.h"
#include "render/vulkan/mesh.h"
#include "render/vulkan/passes/bloom_pass.h"
#include "render/vulkan/passes/depth_prepass.h"
#include "render/vulkan/passes/frame_context.h"
#include "render/vulkan/passes/mesh_pass.h"
#include "render/vulkan/passes/motion_blur_pass.h"
#include "render/vulkan/passes/overlay_pass.h"
#include "render/vulkan/passes/present_pass.h"
#include "render/vulkan/passes/reticle_pass.h"
#include "render/vulkan/passes/scope_lens_pass.h"
#include "render/vulkan/passes/shadow_pass.h"
#include "render/vulkan/passes/skybox_pass.h"
#include "render/vulkan/passes/ssao_pass.h"
#include "render/vulkan/passes/tonemap_pass.h"
#include "render/vulkan/passes/ui_pass.h"
#include "render/vulkan/render_targets.h"
#include "render/vulkan/samplers.h"
#include "render/vulkan/shader_programs.h"
#include "render/vulkan/swapchain.h"
#include "render/vulkan/texture_cache.h"
#include "render/vulkan/vulkan_context.h"

// How a frame is built
// --------------------
// VulkanRenderer::RecordCommands() is the wiring: it records the passes in
// order, each wrapped in a GPU timer scope:
//
//   shadow cascades      ShadowPass        (sun depth, 4 layers)
//   depth pre-pass       DepthPrepass      draw list -> depth
//   ssao                 SsaoPass          depth -> ssao (half res, blurred)
//   scope picture        RecordScopeView   SceneView::scope only: the same
//                                          DepthPrepass, MeshPass and
//                                          SkyboxPass from the scope's
//                                          camera into the scope picture
//                                          (shadow map and IBL shared, no
//                                          SSAO, the held weapon left out)
//   scene (HDR)          BeginScenePass -> MeshPass (depth EQUAL),
//                        SkyboxPass, ReticlePass, ScopeLensPass ->
//                        EndScenePass
//   motion blur          MotionBlurPass    weapon items -> velocity -> tile
//                                          max; hdr blurred in place (via a
//                                          copy); then ResumeScenePass ->
//                                          ReticlePass, ScopeLensPass ->
//                                          EndScenePass. Only in frames with
//                                          weapon motion blur, where the
//                                          sights move here from the scene
//                                          pass.
//   bloom                BloomPass         hdr -> bloom mip chain
//   tonemap              TonemapPass       hdr + bloom -> ldr (sRGB + luma)
//   overlay              OverlayPass       debug lines + selection outline
//                                          -> ldr (editor; skipped if empty)
//   present              PresentPass       ldr -> swapchain (FXAA), or ->
//                                          the editor viewport image
//   ui                   RecordUiPass      ImGui on the swapchain, -> present
//   readbacks            (copies)          RequestViewportReadback() /
//                                          RequestOutputReadback() copies
//
// Headless (RendererOptions::window null) there is no surface and no
// swapchain: an offscreen output image of RendererOptions::headless_extent
// stands in for the swapchain image (same passes, no acquire, no present).
//
// The visible submeshes are culled and sorted once per frame (DrawList) and
// shared by the depth pre-pass and the mesh pass. Which passes run is the
// frame's FramePlan (render/frame_plan.h): disabled effects are skipped
// entirely.
//
// A pass is a class (or a pair of functions) in src/render/vulkan/passes/
// with its
// own shaders. It gets the shared state it needs in its constructor
// (VulkanContext, ShaderPrograms for hot-reloadable pipelines, the
// FrameGlobals set layout, ...) and records with a FrameContext. To add one:
//   1. Write passes/<name>_pass.{h,cc} and shaders/<name>.slang, and list the
//      program in shaders/programs.cmake (shaders/README.md). A full-screen
//      effect wraps a FullscreenPass (render/vulkan/fullscreen_pass.h): it
//      owns the
//      input descriptor sets and the pipeline; set its inputs in a
//      BindTargets() called at startup and after every swapchain rebuild.
//   2. Add a GpuScope (render/gpu_scope.h) and a line in RecordCommands().
//   3. Add the sources to src/render/vulkan/CMakeLists.txt. Window-sized
//      images belong in RenderTargets; scene-wide inputs (environment cube,
//      BRDF LUT, AO) are already bindings of FrameGlobals' set 0
//      (render/vulkan/frame_globals.h).
// Each pass does its own image barriers: it knows the layout its inputs come
// in (documented on the pass) and the scope of the previous frame's last use
// of an image it overwrites (all frames in flight share the render targets).
//
// Layouts between passes: depth is DEPTH_READ_ONLY after the pre-pass (and
// stays so through the scene and motion blur passes); ssao, hdr, bloom,
// ldr and the motion blur images are SHADER_READ_ONLY whenever they are not
// being written or copied (hdr is TRANSFER_SRC for the motion blur's copy);
// the shadow map is
// DEPTH_READ_ONLY; the swapchain image is COLOR_ATTACHMENT after the present
// pass and PRESENT_SRC after the UI.

namespace blue::render::vulkan {

// The Vulkan backend of render::Renderer: draws scenes with a forward PBR
// pipeline into an HDR target, tonemaps and presents them to a window (or,
// headless, into an offscreen output image). Owns meshes, materials and
// textures. Every public function has the contract of the Renderer function
// of the same name (render/renderer.h). Created by CreateVulkanRenderer().
// Neither copyable nor movable. Not thread-safe.
class VulkanRenderer final : public RendererBackend {
 public:
  // Takes `context`, which must have a device (VulkanContext::has_device()).
  VulkanRenderer(const RendererOptions& options,
                 std::unique_ptr<VulkanContext> context);
  ~VulkanRenderer() override;

  [[nodiscard]] Backend backend() const override { return Backend::kVulkan; }
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
    return ui_renderer_.Texture(id, textures_);
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
  void SetVsync(bool enabled) override;
  void ReloadChangedShaders() override { shader_programs_.ReloadChanged(); }
  [[nodiscard]] std::string_view last_shader_error() const override {
    return shader_programs_.last_error();
  }

  [[nodiscard]] bool vsync() const override { return vsync_; }
  [[nodiscard]] std::string_view device_name() const override {
    return context_.device_name();
  }
  [[nodiscard]] core::Extent2D output_extent() const override;
  // No temporal scaler: the scene is rendered at the output size of its
  // target (the window or the editor viewport).
  [[nodiscard]] core::Extent2D render_extent() const override;
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
    const VulkanContext::ValidationCounts counts =
        context_.validation_counts();
    return {.enabled = context_.validation(),
            .errors = counts.errors,
            .warnings = counts.warnings};
  }

  void InitImGuiRenderer() const override;
  void ShutdownImGuiRenderer() const override;

  // Vulkan-only access for tests and the render test harness.
  [[nodiscard]] const VulkanContext& context() const { return context_; }
  [[nodiscard]] bool headless() const { return !swapchain_.has_value(); }
  // The format of the window output: the swapchain's, or the output
  // image's when headless.
  [[nodiscard]] VkFormat output_format() const;

 private:
  using Cascades = std::array<lighting::Cascade, lighting::kCascadeCount>;

  // Per-frame-in-flight command recording and synchronization.
  struct Frame {
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    VkFence in_flight = VK_NULL_HANDLE;
    VkSemaphore image_acquired = VK_NULL_HANDLE;
  };

  // Where a frame's tonemapped scene goes.
  enum class Output {
    kSwapchain,  // The window (DrawFrame()).
    kViewport,   // viewport_image_, with the UI alone on the window.
  };

  // What a readback copies.
  enum class ReadbackSource : uint8_t {
    kViewport,  // viewport_image_ (RequestViewportReadback()).
    kOutput,    // The window output (RequestOutputReadback()).
  };

  void CreateFrames();
  // Destroys the retired meshes and images no frame in flight can use any
  // more. Call after the current frame's fence was waited on.
  void DestroyRetired();
  // Destroys every retired mesh and image if no frame is in flight any more
  // (all fences signalled; does not wait). Keeps releases moving while
  // frames are skipped (minimized window, out-of-date swapchain).
  void DestroyRetiredIfIdle();
  // Points the passes that read render targets at the current ones.
  void BindTargets();
  [[nodiscard]] bool RebuildSwapchain();
  // Blocks until no frame in flight runs on the GPU any more (their fences),
  // so that render targets and their descriptor sets can be replaced.
  void WaitForFramesInFlight() const;
  // Recreates the viewport image and the render targets at `size`.
  void ResizeViewport(VkExtent2D size);
  // The swapchain's, or the output image's when headless.
  [[nodiscard]] VkExtent2D output_vk_extent() const;
  // The body of DrawFrame() and DrawViewportFrame().
  void DrawFrameTo(const SceneView& view, ImDrawData* ui, Output output);

  // Acquires the next swapchain image for `frame`. Returns nullopt, marking
  // the swapchain dirty, when it is out of date and nothing can be drawn.
  [[nodiscard]] std::optional<uint32_t> AcquireImage(const Frame& frame);
  // Submits the recorded commands of `frame` and presents `image_index`
  // (headless, nullopt: only submits).
  void SubmitAndPresent(const Frame& frame,
                        std::optional<uint32_t> image_index);
  // Records every pass of the frame described by `ctx` (see the top of this
  // file, and ctx.plan); `scope_ctx` is the scope picture's, or null.
  void RecordCommands(const Frame& frame, const FrameContext& ctx,
                      const FrameContext* scope_ctx, ImDrawData* ui,
                      Output output);
  // Creates the scope picture and its depth (kMaxScopeResolution square) the
  // first time a frame has a scope; they live as long as the renderer.
  void EnsureScopeTargets();
  // Records the scope picture: depth pre-pass, meshes and sky of
  // `scope_ctx`.
  void RecordScopeView(const FrameContext& scope_ctx);
  // Draws the sights (reflex dot, scope eyepiece) of `ctx`; inside the
  // scene pass.
  void DrawSights(const FrameContext& ctx) const;
  // The image a readback copies, in `layout` (left so afterwards) after
  // `last_write`.
  struct ReadbackImage {
    VkImage image = VK_NULL_HANDLE;
    VkExtent2D extent{};
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    BarrierScope last_write;
    // B8G8R8A8 (a swapchain): channels are swapped when taken.
    bool bgra = false;
  };
  // Records the copy of `image` for every readback of `source` not recorded
  // yet.
  void RecordReadbacks(VkCommandBuffer cmd, ReadbackSource source,
                       const ReadbackImage& image);
  // Computes the world-space bounds of every draw item into world_bounds_.
  void ComputeWorldBounds(const SceneView& view);

  // Declared first: everything below uses the context.
  std::unique_ptr<VulkanContext> context_owner_;
  VulkanContext& context_;
  const platform::Window* window_;  // Null when headless.
  VkExtent2D headless_extent_;
  bool vsync_ = true;
  bool swapchain_dirty_ = false;
  // Empty when headless.
  std::optional<Swapchain> swapchain_;
  Samplers samplers_;
  RenderTargets targets_;
  FrameGlobals frame_globals_;
  TextureCache textures_;
  MaterialSystem materials_;
  ShaderPrograms shader_programs_;
  Environment environment_;
  // Declared before the passes that borrow it.
  std::vector<Mesh> meshes_;
  // Device memory of the resident meshes in meshes_ and their count.
  VkDeviceSize mesh_bytes_ = 0;
  uint32_t mesh_count_ = 0;
  // Released GPU objects waiting until no frame in flight uses them
  // (ReleaseMesh(), ReleaseTextures()); declared after the context so they
  // are destroyed before it.
  template <typename T>
  struct Retired {
    T object;
    uint64_t last_frame = 0;  // The last frame serial that may use it.
  };
  std::vector<Retired<Mesh>> retired_meshes_;
  std::vector<Retired<Image>> retired_images_;
  // The meshes and materials as the draw lists read them.
  DrawListSource draw_source_{materials_, meshes_};
  DrawList draw_list_;
  // The scope view's culled draw list and its picture and depth
  // (EnsureScopeTargets(); empty until a frame has a scope).
  DrawList scope_draw_list_;
  Image scope_picture_;
  Image scope_depth_;

  ShadowPass shadow_pass_;
  DepthPrepass depth_prepass_;
  SsaoPass ssao_pass_;
  MeshPass mesh_pass_;
  SkyboxPass skybox_pass_;
  ReticlePass reticle_pass_;
  ScopeLensPass scope_lens_pass_;
  MotionBlurPass motion_blur_pass_;
  BloomPass bloom_pass_;
  TonemapPass tonemap_pass_;
  OverlayPass overlay_pass_;
  PresentPass present_pass_;
  // The present pass into viewport_image_'s format; created by the first
  // PrepareViewport().
  std::optional<PresentPass> viewport_present_pass_;

  // The window output when headless (stands in for the swapchain image):
  // TRANSFER_SRC_OPTIMAL between frames.
  Image output_image_;
  // Editor viewport output (PrepareViewport()); empty in the game.
  Image viewport_image_;
  // Mutable: (un)registering the image with ImGui's backend
  // (InitImGuiRenderer(), ShutdownImGuiRenderer()) changes its texture.
  mutable ViewportImage viewport_;
  // ImGui's Vulkan backend and the ImGui textures handed out (ui_texture(),
  // ViewportImage::texture); registering a texture is not a visible change.
  mutable UiRenderer ui_renderer_;
  // frame_serial_ at the last viewport recreation.
  uint64_t viewport_resized_at_ = 0;
  // Requested copies (RequestViewportReadback(), RequestOutputReadback()).
  struct Readback {
    uint64_t ticket = 0;
    ReadbackSource source = ReadbackSource::kViewport;
    // The frame that records the copy (0 until recorded) and its slot in
    // frames_, whose fence tells when the copy is done.
    uint64_t frame_serial = 0;
    size_t frame_slot = 0;
    VkExtent2D extent{};
    bool bgra = false;
    Buffer buffer;
  };
  std::vector<Readback> readbacks_;
  uint64_t next_readback_ticket_ = 1;

  std::array<Frame, kFramesInFlight> frames_;
  size_t frame_index_ = 0;
  // Number of the frame being recorded; counts every recorded frame from 1.
  uint64_t frame_serial_ = 0;
  // World-space bounds of the current frame's draw items, parallel to
  // SceneView::draws; kept as a member to reuse its allocation.
  std::vector<camera::Aabb> world_bounds_;
  RenderStats stats_;
  GpuTimer gpu_timer_;
};

}  // namespace blue::render::vulkan

namespace blue::render {

// Returns the Vulkan renderer for `options`, or null when Vulkan has no
// suitable device (CreateBackend() then tries another backend).
[[nodiscard]] std::unique_ptr<RendererBackend> CreateVulkanRenderer(
    const RendererOptions& options);

}  // namespace blue::render

#endif  // BLUE_RENDER_VULKAN_VULKAN_RENDERER_H_
