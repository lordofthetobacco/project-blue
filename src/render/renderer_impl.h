#ifndef BLUE_RENDER_RENDERER_IMPL_H_
#define BLUE_RENDER_RENDERER_IMPL_H_

#include "render/metal_types.h"

#include <imgui.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "asset/mesh_data.h"
#include "core/extent.h"
#include "platform/window.h"
#include "render/buffer.h"
#include "render/frame_config.h"
#include "render/fullscreen_pass.h"
#include "render/gpu_timer.h"
#include "render/image.h"
#include "render/material_system.h"
#include "render/mesh.h"
#include "render/metal_context.h"
#include "render/passes/bloom_pass.h"
#include "render/passes/frame_context.h"
#include "render/passes/present_pass.h"
#include "render/passes/tonemap_pass.h"
#include "render/passes/ui_pass.h"
#include "render/render_targets.h"
#include "render/renderer.h"
#include "render/samplers.h"
#include "render/scene_view.h"
#include "render/shader_programs.h"
#include "render/swapchain.h"
#include "render/texture_cache.h"
#include "shader/shader_compiler.h"

// Internal to src/render: the state behind render::Renderer.
//
// How a frame is built
// --------------------
// RendererImpl::RecordCommands() is the wiring: it records the passes in
// order into the frame's one command buffer, each wrapped in a GPU timer
// scope. The target design (Stages 4-5 of the Metal port bring back the
// passes before bloom; until then a test pattern stands in for the scene,
// see RecordCommands()):
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
//   present              PresentPass       ldr -> drawable (FXAA), or ->
//                                          the editor viewport image
//   ui                   RecordUiPass      ImGui on the drawable
//
// The visible submeshes are culled and sorted once per frame (DrawList) and
// shared by the depth pre-pass and the mesh pass. Disabled effects
// (SceneView::post) are skipped entirely.
//
// A pass is a class (or a pair of functions) in src/render/passes/ with its
// own shaders. It gets the shared state it needs in its constructor
// (MetalContext, ShaderPrograms for hot-reloadable pipelines, ...) and
// records with a FrameContext, opening its encoders through
// render/encoders.h. To add one:
//   1. Write passes/<name>_pass.{h,mm} and its shaders/<name>.*.metal.
//   2. Add a GpuScope (render/gpu_scope.h) and a line in RecordCommands().
//   3. Add the sources to CMakeLists.txt. Window-sized images belong in
//      RenderTargets.
// There are no image layouts, barriers or descriptor sets: Metal orders the
// encoders of the single queue and tracks the hazards between them, and
// passes bind their inputs on their encoders (indices in shaders/bindings.h).
// The window's drawable is acquired as late as possible, just before the
// first pass that renders to it; when none is available the passes that
// need it are skipped and the frame is still committed. The editor viewport
// image is RGBA8Unorm_sRGB, the same encoding as the drawable.

namespace blue::render {

// Everything behind Renderer; see Renderer for the meaning of the methods it
// forwards to. Neither copyable nor movable. Not thread-safe.
struct RendererImpl {
  // Per-frame-in-flight state: the frame slot's last command buffer, waited
  // on (waitUntilCompleted) before the slot records again.
  struct Frame {
    id<MTLCommandBuffer> command_buffer = nil;
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
    core::Extent2D extent{};
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
  ViewportImage PrepareViewport(core::Extent2D size);
  uint64_t RequestViewportReadback();
  ReadbackStatus TakeViewportReadback(uint64_t ticket, ViewportPixels& pixels);
  void InitImGuiRenderer();
  void ShutdownImGuiRenderer();

  // Destroys the retired meshes and images no frame in flight can use any
  // more. Call after the current frame slot's command buffer was waited on.
  void DestroyRetired();
  [[nodiscard]] bool RebuildSwapchain();
  // Blocks until `frame`'s last command buffer, if any, has completed; a GPU
  // error in it is fatal.
  static void WaitForFrame(const Frame& frame);
  // Blocks until no frame in flight runs on the GPU any more, so that render
  // targets can be replaced.
  void WaitForFramesInFlight() const;
  // Recreates the viewport image and the render targets at `size`.
  void ResizeViewport(core::Extent2D size);
  // Points every pass at the current render targets; call after they were
  // created or resized.
  void BindTargets();
  // The body of DrawFrame() and DrawViewportFrame().
  void DrawFrameTo(const SceneView& view, ImDrawData* ui, Output output);

  // Acquires the window's drawable into ctx.drawable. Returns false, marking
  // the swapchain dirty, when none was available.
  [[nodiscard]] bool AcquireDrawable(FrameContext& ctx);
  // Records every pass of the frame described by `ctx` (see the top of this
  // file); acquires the drawable on the way.
  void RecordCommands(FrameContext& ctx, ImDrawData* ui, Output output);
  // Records the copy of the viewport image, after it was rendered, for every
  // readback not recorded yet.
  void RecordViewportReadbacks(const FrameContext& ctx);

  const platform::Window& window_;
  // The directory of the MSL sources, compiled with `shader_mode_`.
  std::string shader_dir_;
  shader::CompileMode shader_mode_;
  MetalContext context_;
  bool vsync_ = true;
  bool swapchain_dirty_ = false;
  Swapchain swapchain_;
  Samplers samplers_;
  RenderTargets targets_;
  // TODO(metal-stage4): FrameGlobals frame_globals_;
  TextureCache textures_;
  MaterialSystem materials_;
  // Declared before the passes that register their programs with it.
  ShaderPrograms shader_programs_;
  // TODO(metal-stage4): Environment environment_;
  // Declared before the passes that borrow it.
  std::vector<Mesh> meshes_;
  // Released GPU objects waiting until no frame in flight uses them
  // (ReleaseMesh(), ReleaseTextures(), replaced viewport images); declared
  // after the context so they are destroyed before it.
  std::vector<Retired<Mesh>> retired_meshes_;
  std::vector<Retired<Image>> retired_images_;
  // TODO(metal-stage4): DrawList draw_list_;

  // TODO(metal-stage4): replaced by the scene pass. Writes a uv test
  // pattern (shaders/debug_gradient.frag.metal) into the HDR image.
  FullscreenPass debug_gradient_pass_;
  BloomPass bloom_pass_;
  TonemapPass tonemap_pass_;
  // Into the drawable.
  PresentPass present_pass_;
  // Into viewport_image_; created by the first PrepareViewport().
  std::optional<PresentPass> viewport_present_pass_;
  // TODO(metal-stage4): DepthPrepass depth_prepass_, MeshPass mesh_pass_,
  // SkyboxPass skybox_pass_.
  // TODO(metal-stage5): ShadowPass shadow_pass_, SsaoPass ssao_pass_,
  // ReticlePass reticle_pass_, OverlayPass overlay_pass_.
  // The ImGui backend.
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
  // TODO(metal-stage4): world_bounds_, the world-space bounds of the current
  // frame's draw items, parallel to SceneView::draws.
  RenderStats stats_;
  GpuTimer gpu_timer_;
};

}  // namespace blue::render

#endif  // BLUE_RENDER_RENDERER_IMPL_H_
