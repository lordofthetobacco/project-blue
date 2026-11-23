#ifndef BLUE_RENDER_METAL_RENDERER_IMPL_H_
#define BLUE_RENDER_METAL_RENDERER_IMPL_H_

#include "render/metal/metal_types.h"

#include <imgui.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "asset/mesh_data.h"
#include "camera/frustum.h"
#include "core/extent.h"
#include "platform/window.h"
#include "render/metal/buffer.h"
#include "render/metal/draw_list.h"
#include "render/metal/environment.h"
#include "render/frame_config.h"
#include "render/metal/frame_globals.h"
#include "render/metal/gpu_timer.h"
#include "render/metal/image.h"
#include "render/metal/material_system.h"
#include "render/memory_stats.h"
#include "render/metal/mesh.h"
#include "render/metal/metal_context.h"
#include "render/metal/passes/bloom_pass.h"
#include "render/metal/passes/depth_prepass.h"
#include "render/metal/passes/frame_context.h"
#include "render/metal/passes/mesh_pass.h"
#include "render/metal/passes/motion_blur_pass.h"
#include "render/metal/passes/motion_vector_pass.h"
#include "render/metal/passes/overlay_pass.h"
#include "render/metal/passes/present_pass.h"
#include "render/metal/passes/reticle_pass.h"
#include "render/metal/passes/scope_lens_pass.h"
#include "render/metal/passes/shadow_pass.h"
#include "render/metal/passes/skybox_pass.h"
#include "render/metal/passes/ssao_pass.h"
#include "render/metal/passes/temporal_scaler_pass.h"
#include "render/metal/passes/tonemap_pass.h"
#include "render/metal/passes/ui_pass.h"
#include "render/metal/recording.h"
#include "render/metal/render_targets.h"
#include "render/renderer.h"
#include "render/metal/samplers.h"
#include "render/scene_view.h"
#include "render/metal/shader_programs.h"
#include "render/metal/swapchain.h"
#include "render/metal/texture_cache.h"
#include "shader/metal/shader_compiler.h"

// Internal to src/render: the state behind render::Renderer.
//
// How a frame is built
// --------------------
// RendererImpl::DrawFrameTo() waits for the frame slot's previous work
// (its Submission), collects its GPU times, resets the slot's command
// allocator and transient constants, begins the slot's command buffer on
// the allocator, writes the frame's uniforms and calls RecordCommands(), the
// wiring: it records the passes in order into the frame's one command
// buffer, each between GpuTimer::BeginScope() and EndScope() of its
// GpuScope. Every pass opens its own render encoders (copy encoders for
// copies) through render/encoders.h:
//
//   shadow cascades      ShadowPass        sun depth, one encoder per
//                                          cascade layer (4)
//   depth pre-pass       DepthPrepass      draw list -> depth
//   motion vectors       MotionVectorPass  temporal scaler only: depth and
//                                          the moving items -> where each
//                                          pixel was last frame
//   ssao                 SsaoPass          depth -> ssao, half res:
//                                          occlusion + two blur encoders
//   scope picture        RecordScopeView() SceneView::scope only: the same
//                                          DepthPrepass, MeshPass and
//                                          SkyboxPass from the scope's
//                                          camera into the scope picture
//                                          (shadow map and IBL shared, no
//                                          SSAO, the held weapon left out)
//   scene (HDR)          BeginScenePass()  one encoder shared by MeshPass
//                                          (depth Equal), SkyboxPass,
//                                          ReticlePass and ScopeLensPass ->
//                                          EndScenePass()
//   motion blur          MotionBlurPass    weapon items -> velocity -> tile
//                                          max; hdr blurred in place (via a
//                                          copy); then ResumeScenePass() ->
//                                          ReticlePass, ScopeLensPass ->
//                                          EndScenePass(). Only in frames
//                                          with weapon motion blur, where
//                                          the sights move here from the
//                                          scene pass.
//   upscale              TemporalScalerPass  temporal scaler only: the
//                                          sights' reactive mask, then
//                                          MetalFX: hdr + depth + motion
//                                          vectors -> upscaled hdr
//   bloom                BloomPass         hdr -> bloom mips, an encoder per
//                                          down- and up-sample step
//   tonemap              TonemapPass       hdr + bloom -> ldr (sRGB + luma)
//   overlay              OverlayPass       debug lines + selection outline
//                                          -> ldr (editor; skipped if empty)
//   present              PresentPass       ldr -> drawable (FXAA), or ->
//                                          the editor viewport image (plus
//                                          a copy per viewport readback)
//   ui                   RecordUiPass()    ImGui on the drawable
//
// Two sizes: everything up to the weapon motion blur renders at the render
// extent, everything from bloom on at the output extent (RenderTargets).
// They differ only while an upscaling mode is on (SceneView::post.upscaling,
// render/temporal_math.h); then the scene is rasterized with a sub-pixel
// jitter that changes every frame, and the scaler turns the jittered frames
// into the anti-aliased image the post chain reads
// (RenderTargets::post_hdr()). With Upscaling::kOff nothing of this exists
// and the frame is recorded exactly as before. A mode or size change
// recreates the targets and the scaler with the GPU idle (ConfigureTargets()).
//
// The visible submeshes are culled and sorted once per frame (DrawList) and
// shared by the depth pre-pass and the mesh pass. Disabled effects
// (SceneView::post) are skipped entirely; with SSAO off its target is
// cleared to white once, outside any scope.
//
// A pass is a class (or a pair of functions) in src/render/passes/ with its
// own shaders. It gets the shared state it needs in its constructor
// (MetalContext, ShaderPrograms for hot-reloadable pipelines, ...) and
// records with a FrameContext. To add one:
//   1. Write passes/<name>_pass.{h,mm} and its shaders/<name>.vert.metal /
//      <name>.frag.metal (shared declarations in shaders/<name>.h). A pass
//      that runs one fragment shader over a whole target uses a
//      FullscreenPass: textures at kTextureInput0 + n with the sampler at
//      kSamplerInput (SetInputs()), constants at kBufferPass, vertex shader
//      shaders/fullscreen.vert.metal. Every index comes from
//      shaders/bindings.h; per-pass constants are a struct mirrored in C++
//      with a static_assert on its size.
//   2. Open encoders and bind only through render/encoders.h, whose header
//      comment is the rule book (BeginRenderPass(), BeginCopy(), Bind*()):
//      it takes care of barriers, the argument table and residency.
//   3. Add a GpuScope (render/gpu_scope.h) and a timed(...) line in
//      RecordCommands().
//   4. Add the .mm to the blue_render list in CMakeLists.txt. Window-sized
//      images belong in RenderTargets (re-bound in BindTargets()).
// This is the Metal 4 core API: nothing is tracked or retained for us. The
// single queue runs the encoders in order, each behind a barrier on the
// output of the earlier ones; resources are resident through the context's
// residency set; and what a frame uses stays alive until the frame has
// completed (the retire lists below). When the frame is committed the
// window's drawable is presented through the queue (MetalContext::Commit()).
// The drawable is acquired as late as possible, just before the first pass
// that renders to it; when none is available the passes that need it are
// skipped and the frame is still committed. The editor viewport image is
// RGBA8Unorm_sRGB, the same encoding as the drawable.

namespace blue::render::metal {

// Everything behind Renderer; see Renderer for the meaning of the methods it
// forwards to. Neither copyable nor movable. Not thread-safe.
struct RendererImpl {
  // Per-frame-in-flight state. The slot's command buffer records into the
  // slot's allocator, which is reset (with the constants) once the slot's
  // last work has completed, right before it records again.
  struct Frame {
    id<MTL4CommandAllocator> allocator = nil;
    id<MTL4CommandBuffer> command_buffer = nil;
    // Never null after construction.
    std::unique_ptr<TransientArena> constants;
    // The completion of the last commit; null before the first.
    std::shared_ptr<const Submission> submission;
    // The drawable that commit presented, which nothing else keeps alive
    // until the GPU has rendered to it.
    id<CAMetalDrawable> drawable = nil;
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

  MeshId UploadMesh(std::span<const asset::Vertex> vertices,
                    std::span<const asset::Index> indices);
  void ReleaseMesh(MeshId id);
  void ReleaseTextures(std::span<const TextureId> ids);
  void FlushReleases();
  [[nodiscard]] MemoryStats memory_stats() const;
  void SetVsync(bool enabled);
  ViewportImage PrepareViewport(core::Extent2D size);
  uint64_t RequestViewportReadback();
  ReadbackStatus TakeViewportReadback(uint64_t ticket, ViewportPixels& pixels);
  void InitImGuiRenderer();
  void ShutdownImGuiRenderer();

  // Destroys the retired meshes and images no frame in flight can use any
  // more. Call after the current frame slot's last work was waited for.
  void DestroyRetired();
  // Destroys every retired mesh and image if no frame is in flight any more
  // (every slot's last work completed; does not wait). Keeps releases
  // moving while frames are skipped (minimized window, no swapchain).
  void DestroyRetiredIfIdle();
  [[nodiscard]] bool RebuildSwapchain();
  // Blocks until `frame`'s last committed work, if any, has completed; a
  // GPU error in it is fatal.
  static void WaitForFrame(const Frame& frame);
  // Blocks until no frame in flight runs on the GPU any more, so that render
  // targets can be replaced.
  void WaitForFramesInFlight() const;
  // Recreates the viewport image and the render targets at `size`.
  void ResizeViewport(core::Extent2D size);
  // Gives every frame slot a new command buffer. No slot may be recording
  // or in flight.
  void CreateFrameCommandBuffers();
  // Recreates the render targets for an output of `output` pixels in
  // upscaling mode `mode` (kOff when unsupported), with the scaler, the
  // texture LOD bias and every pass's bindings to match, and starts a new
  // temporal history. The GPU must be idle.
  void ConfigureTargets(core::Extent2D output, Upscaling mode);
  // Points every pass at the current render targets; call after they were
  // created or resized.
  void BindTargets();
  // The body of DrawFrame() and DrawViewportFrame().
  void DrawFrameTo(const SceneView& view, ImDrawData* ui, Output output);

  // Acquires the window's drawable into ctx.drawable. Returns false, marking
  // the swapchain dirty, when none was available.
  [[nodiscard]] bool AcquireDrawable(FrameContext& ctx);
  // Records every pass of the frame described by `ctx` (see the top of this
  // file); `scope_ctx` is the scope picture's, or null. Acquires the
  // drawable on the way.
  void RecordCommands(FrameContext& ctx, const FrameContext* scope_ctx,
                      bool reset_history, ImDrawData* ui, Output output);
  // Creates the scope picture and its depth (kMaxScopeResolution square) the
  // first time a frame has a scope; they live as long as the renderer.
  void EnsureScopeTargets();
  // Records the scope picture: depth pre-pass, meshes and sky of
  // `scope_ctx`.
  void RecordScopeView(const FrameContext& scope_ctx);
  // Records the reactive mask of the sights of `ctx` for the temporal
  // scaler.
  void RecordReactiveMask(const FrameContext& ctx) const;
  // Draws the sights (reflex dot, scope eyepiece) of `ctx` on `encoder`, the
  // scene pass's.
  void DrawSights(const FrameContext& ctx,
                  id<MTL4RenderCommandEncoder> encoder) const;
  // Computes the world-space bounds of every draw item into world_bounds_.
  void ComputeWorldBounds(const SceneView& view);
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
  FrameGlobals frame_globals_;
  TextureCache textures_;
  MaterialSystem materials_;
  // Declared before the passes that register their programs with it.
  ShaderPrograms shader_programs_;
  // Declared after frame_globals_, which it binds its textures into.
  Environment environment_;
  // Declared before the passes that borrow it.
  std::vector<Mesh> meshes_;
  // GPU memory of the resident meshes in meshes_ and their count.
  uint64_t mesh_bytes_ = 0;
  uint32_t mesh_count_ = 0;
  // Released GPU objects waiting until no frame in flight uses them
  // (ReleaseMesh(), ReleaseTextures(), replaced viewport images); declared
  // after the context so they are destroyed before it.
  std::vector<Retired<Mesh>> retired_meshes_;
  std::vector<Retired<Image>> retired_images_;
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
  MotionVectorPass motion_vector_pass_;
  TemporalScalerPass temporal_scaler_pass_;
  BloomPass bloom_pass_;
  TonemapPass tonemap_pass_;
  OverlayPass overlay_pass_;
  // Into the drawable.
  PresentPass present_pass_;
  // Into viewport_image_; created by the first PrepareViewport().
  std::optional<PresentPass> viewport_present_pass_;
  // The ImGui backend.
  UiRenderer ui_;

  // Editor viewport output (PrepareViewport()); empty in the game.
  Image viewport_image_;
  ViewportImage viewport_;
  // frame_serial_ at the last viewport recreation.
  uint64_t viewport_resized_at_ = 0;
  std::vector<Readback> readbacks_;
  uint64_t next_readback_ticket_ = 1;

  // The upscaling mode the targets are configured for.
  Upscaling upscaling_ = Upscaling::kOff;
  // Counts the frames recorded with the scaler: the jitter phase.
  uint64_t temporal_frame_ = 0;
  // False until a frame was recorded with the current targets, and after a
  // frame was skipped: the scaler's history is not the next frame's past.
  bool temporal_history_valid_ = false;

  std::array<Frame, kFramesInFlight> frames_;
  size_t frame_index_ = 0;
  // Number of the frame being recorded; counts every recorded frame from 1.
  uint64_t frame_serial_ = 0;
  // World-space bounds of the current frame's draw items, parallel to
  // SceneView::draws.
  std::vector<camera::Aabb> world_bounds_;
  RenderStats stats_;
  GpuTimer gpu_timer_;
};

}  // namespace blue::render::metal

#endif  // BLUE_RENDER_METAL_RENDERER_IMPL_H_
