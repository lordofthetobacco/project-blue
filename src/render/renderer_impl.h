#ifndef BLUE_RENDER_RENDERER_IMPL_H_
#define BLUE_RENDER_RENDERER_IMPL_H_

#include <imgui.h>
#include <vulkan/vulkan.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "asset/mesh_data.h"
#include "camera/frustum.h"
#include "core/extent.h"
#include "lighting/cascades.h"
#include "platform/window.h"
#include "render/buffer.h"
#include "render/draw_list.h"
#include "render/environment.h"
#include "render/frame_config.h"
#include "render/frame_globals.h"
#include "render/gpu_timer.h"
#include "render/image.h"
#include "render/material_system.h"
#include "render/mesh.h"
#include "render/passes/bloom_pass.h"
#include "render/passes/depth_prepass.h"
#include "render/passes/frame_context.h"
#include "render/passes/mesh_pass.h"
#include "render/passes/overlay_pass.h"
#include "render/passes/present_pass.h"
#include "render/passes/reticle_pass.h"
#include "render/passes/shadow_pass.h"
#include "render/passes/skybox_pass.h"
#include "render/passes/ssao_pass.h"
#include "render/passes/tonemap_pass.h"
#include "render/passes/ui_pass.h"
#include "render/render_targets.h"
#include "render/renderer.h"
#include "render/samplers.h"
#include "render/scene_view.h"
#include "render/shader_programs.h"
#include "render/swapchain.h"
#include "render/texture_cache.h"
#include "render/vulkan_context.h"
#include "shader/shader_compiler.h"

// Internal to src/render: the state behind render::Renderer.
//
// How a frame is built
// --------------------
// RendererImpl::RecordCommands() is the wiring: it records the passes in
// order, each wrapped in a GPU timer scope:
//
//   shadow cascades      ShadowPass        (sun depth, 4 layers)
//   depth pre-pass       DepthPrepass      draw list -> depth
//   ssao                 SsaoPass          depth -> ssao (half res, blurred)
//   scene (HDR)          BeginScenePass -> MeshPass (depth EQUAL),
//                        SkyboxPass, ReticlePass -> EndScenePass
//   bloom                BloomPass         hdr -> bloom mip chain
//   tonemap              TonemapPass       hdr + bloom -> ldr (sRGB + luma)
//   overlay              OverlayPass       debug lines + selection outline
//                                          -> ldr (editor; skipped if empty)
//   present              PresentPass       ldr -> swapchain (FXAA), or ->
//                                          the editor viewport image
//   ui                   RecordUiPass      ImGui on the swapchain, -> present
//
// The visible submeshes are culled and sorted once per frame (DrawList) and
// shared by the depth pre-pass and the mesh pass. Disabled effects
// (SceneView::post) are skipped entirely.
//
// A pass is a class (or a pair of functions) in src/render/passes/ with its
// own shaders. It gets the shared state it needs in its constructor
// (VulkanContext, ShaderPrograms for hot-reloadable pipelines, the
// FrameGlobals set layout, ...) and records with a FrameContext. To add one:
//   1. Write passes/<name>_pass.{h,cc} and shaders/<name>.frag. A full-screen
//      effect wraps a FullscreenPass (render/fullscreen_pass.h): it owns the
//      input descriptor sets and the pipeline; set its inputs in a
//      BindTargets() called at startup and after every swapchain rebuild.
//   2. Add a GpuScope (render/gpu_scope.h) and a line in RecordCommands().
//   3. Add the sources to CMakeLists.txt. Window-sized images belong in
//      RenderTargets; scene-wide inputs (environment cube, BRDF LUT, AO) are
//      already bindings of FrameGlobals' set 0 (render/frame_globals.h).
// Each pass does its own image barriers: it knows the layout its inputs come
// in (documented on the pass) and the scope of the previous frame's last use
// of an image it overwrites (all frames in flight share the render targets).
//
// Layouts between passes: depth is DEPTH_READ_ONLY after the pre-pass (and
// stays so through the scene pass); ssao, hdr, bloom and ldr are
// SHADER_READ_ONLY whenever they are not being written; the shadow map is
// DEPTH_READ_ONLY; the swapchain image is COLOR_ATTACHMENT after the present
// pass and PRESENT_SRC after the UI. The editor viewport image is
// SHADER_READ_ONLY_OPTIMAL whenever the UI pass samples it; it is
// R8G8B8A8, _SRGB when the swapchain is.

namespace blue::render {

// Everything behind Renderer; see Renderer for the meaning of the methods it
// forwards to. Neither copyable nor movable. Not thread-safe.
struct RendererImpl {
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

  // A released GPU object waiting until no frame in flight uses it.
  template <typename T>
  struct Retired {
    T object;
    uint64_t last_frame = 0;  // The last frame serial that may use it.
  };

  // Requested viewport copies (RequestViewportReadback()).
  struct Readback {
    uint64_t ticket = 0;
    // The frame that records the copy; 0 until recorded.
    uint64_t frame_serial = 0;
    VkExtent2D extent{};
    Buffer buffer;
  };

  RendererImpl(const platform::Window& window, const std::string& shader_dir,
               shader::CompileMode shader_mode);
  ~RendererImpl();

  RendererImpl(const RendererImpl&) = delete;
  RendererImpl& operator=(const RendererImpl&) = delete;

  MeshId UploadMesh(const asset::MeshData& data);
  void ReleaseMesh(MeshId id);
  void ReleaseTextures(std::span<const TextureId> ids);
  void SetVsync(bool enabled);
  ViewportImage PrepareViewport(VkExtent2D size);
  uint64_t RequestViewportReadback();
  ReadbackStatus TakeViewportReadback(uint64_t ticket, ViewportPixels& pixels);
  void InitImGuiRenderer();
  void ShutdownImGuiRenderer();

  void CreateFrames();
  // Destroys the retired meshes, images and UI textures no frame in flight
  // can use any more. Call after the current frame's fence was waited on.
  void DestroyRetired();
  // Points the passes that read render targets at the current ones.
  void BindTargets();
  [[nodiscard]] bool RebuildSwapchain();
  // Blocks until no frame in flight runs on the GPU any more (their fences),
  // so that render targets and their descriptor sets can be replaced.
  void WaitForFramesInFlight() const;
  // Recreates the viewport image and the render targets at `size`.
  void ResizeViewport(VkExtent2D size);
  // The body of DrawFrame() and DrawViewportFrame().
  void DrawFrameTo(const SceneView& view, ImDrawData* ui, Output output);

  // Acquires the next swapchain image for `frame`. Returns nullopt, marking
  // the swapchain dirty, when it is out of date and nothing can be drawn.
  [[nodiscard]] std::optional<uint32_t> AcquireImage(const Frame& frame);
  // Submits the recorded commands of `frame` and presents `image_index`.
  void SubmitAndPresent(const Frame& frame, uint32_t image_index);
  // Records every pass of the frame described by `ctx` (see the top of this
  // file).
  void RecordCommands(const Frame& frame, const FrameContext& ctx,
                      ImDrawData* ui, Output output);
  // Records the copy of the viewport image (COLOR_ATTACHMENT_OPTIMAL after
  // the present pass, and again afterwards) for every readback not recorded
  // yet.
  void RecordViewportReadbacks(VkCommandBuffer cmd);
  // Computes the world-space bounds of every draw item into world_bounds_.
  void ComputeWorldBounds(const SceneView& view);

  const platform::Window& window_;
  std::string shader_dir_;
  VulkanContext context_;
  bool vsync_ = true;
  bool swapchain_dirty_ = false;
  Swapchain swapchain_;
  Samplers samplers_;
  RenderTargets targets_;
  FrameGlobals frame_globals_;
  TextureCache textures_;
  MaterialSystem materials_;
  ShaderPrograms shader_programs_;
  Environment environment_;
  // Declared before the passes that borrow it.
  std::vector<Mesh> meshes_;
  // Released GPU objects waiting until no frame in flight uses them
  // (ReleaseMesh(), ReleaseTextures()); declared after the context so they
  // are destroyed before it.
  std::vector<Retired<Mesh>> retired_meshes_;
  std::vector<Retired<Image>> retired_images_;
  DrawList draw_list_;

  ShadowPass shadow_pass_;
  DepthPrepass depth_prepass_;
  SsaoPass ssao_pass_;
  MeshPass mesh_pass_;
  SkyboxPass skybox_pass_;
  ReticlePass reticle_pass_;
  BloomPass bloom_pass_;
  TonemapPass tonemap_pass_;
  OverlayPass overlay_pass_;
  PresentPass present_pass_;
  // The present pass into viewport_image_'s format; created by the first
  // PrepareViewport().
  std::optional<PresentPass> viewport_present_pass_;
  // The ImGui backend and the ImGui textures (viewport, ui_texture()).
  UiRenderer ui_;

  // Editor viewport output (PrepareViewport()); empty in the game.
  Image viewport_image_;
  ViewportImage viewport_;
  // frame_serial_ at the last viewport recreation.
  uint64_t viewport_resized_at_ = 0;
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

}  // namespace blue::render

#endif  // BLUE_RENDER_RENDERER_IMPL_H_
