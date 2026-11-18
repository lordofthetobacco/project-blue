#ifndef BLUE_RENDER_RENDERER_H_
#define BLUE_RENDER_RENDERER_H_

#include <imgui.h>
#include <vulkan/vulkan.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "asset/image_decoder.h"
#include "asset/mesh_data.h"
#include "camera/frustum.h"
#include "lighting/cascades.h"
#include "material/material.h"
#include "platform/window.h"
#include "render/draw_list.h"
#include "render/environment.h"
#include "render/frame_config.h"
#include "render/frame_globals.h"
#include "render/gpu_timer.h"
#include "render/material_system.h"
#include "render/mesh.h"
#include "render/passes/bloom_pass.h"
#include "render/passes/depth_prepass.h"
#include "render/passes/frame_context.h"
#include "render/passes/mesh_pass.h"
#include "render/passes/present_pass.h"
#include "render/passes/reticle_pass.h"
#include "render/passes/shadow_pass.h"
#include "render/passes/skybox_pass.h"
#include "render/passes/ssao_pass.h"
#include "render/passes/tonemap_pass.h"
#include "render/render_targets.h"
#include "render/samplers.h"
#include "render/scene_view.h"
#include "render/shader_programs.h"
#include "render/swapchain.h"
#include "render/texture_cache.h"
#include "render/vulkan_context.h"
#include "shader/shader_compiler.h"

// How a frame is built
// --------------------
// Renderer::RecordCommands() is the wiring: it records the passes in order,
// each wrapped in a GPU timer scope:
//
//   shadow cascades      ShadowPass        (sun depth, 4 layers)
//   depth pre-pass       DepthPrepass      draw list -> depth
//   ssao                 SsaoPass          depth -> ssao (half res, blurred)
//   scene (HDR)          BeginScenePass -> MeshPass (depth EQUAL),
//                        SkyboxPass, ReticlePass -> EndScenePass
//   bloom                BloomPass         hdr -> bloom mip chain
//   tonemap              TonemapPass       hdr + bloom -> ldr (sRGB + luma)
//   present              PresentPass       ldr -> swapchain (FXAA)
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
//   2. Add a GpuScope (render/gpu_timer.h) and a line in RecordCommands().
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
// pass and PRESENT_SRC after the UI.

namespace blue::render {

// Draws scenes with a forward PBR pipeline into an HDR target, tonemaps and
// presents them to a window. Owns meshes, materials and textures. Borrows
// `window`, which must outlive the renderer. Startup failures are fatal.
// Neither copyable nor movable. Not thread-safe.
class Renderer {
 public:
  // Compiles the GLSL shaders in `shader_dir` with `shader_mode` and watches
  // them for changes (see ReloadChangedShaders()). A shader that does not
  // compile at startup is fatal.
  Renderer(const platform::Window& window, const std::string& shader_dir,
           shader::CompileMode shader_mode);
  ~Renderer();

  Renderer(const Renderer&) = delete;
  Renderer& operator=(const Renderer&) = delete;

  // Renders and presents one frame. `ui` is drawn on top when non-null.
  void DrawFrame(const SceneView& view, ImDrawData* ui);

  // Uploads `data` and returns its handle. Meshes stay resident for the whole
  // session; unloading is out of scope.
  [[nodiscard]] MeshId UploadMesh(const asset::MeshData& data);

  // Decodes and uploads the textures `materials` reference in one batch
  // (parallel decode). Optional before CreateMaterial(), which would load
  // them one by one.
  void PreloadTextures(std::span<const material::MaterialDesc> materials) {
    textures_.Preload(materials);
  }
  // Creates a material (textures load on first use) and returns its handle.
  // Materials stay resident for the whole session.
  [[nodiscard]] MaterialId CreateMaterial(const material::MaterialDesc& desc) {
    return materials_.Create(desc);
  }
  // Replaces material `id` by `desc` from the next frame on (live editing);
  // frames in flight keep the old version. Call between frames.
  void UpdateMaterial(MaterialId id, const material::MaterialDesc& desc) {
    materials_.Update(id, desc);
  }
  // Returns the current description of `id`, which must be valid.
  [[nodiscard]] const material::MaterialDesc& material(MaterialId id) const {
    return materials_.desc(id);
  }
  [[nodiscard]] size_t material_count() const { return materials_.count(); }
  [[nodiscard]] TextureStats texture_stats() const { return textures_.stats(); }

  // Replaces the environment (sky and image-based lighting) by the
  // equirectangular HDR image `equirect`, scaled by `intensity` and turned
  // by `rotation_radians` about +Y. Waits for the GPU and filters the image
  // (see render::Environment): meant for scene loads, not every frame.
  void SetEnvironment(const asset::ImageDataF& equirect, float intensity,
                      float rotation_radians) {
    context_.WaitIdle();
    environment_.Set(equirect, intensity, rotation_radians);
  }
  // Changes the intensity and rotation of the current environment; cheap.
  void SetEnvironmentParams(float intensity, float rotation_radians) {
    environment_.SetParams(intensity, rotation_radians);
  }

  // Rebuilds the swapchain before the next frame, e.g. after a resize.
  void RequestSwapchainRebuild() { swapchain_dirty_ = true; }
  void SetVsync(bool enabled);

  // Recompiles the shaders whose sources changed and swaps in the new
  // pipelines; a shader that fails to compile keeps its previous version.
  // Cheap when nothing changed, but not free (it stats the watched files):
  // call it a few times per second, not every frame, and not while a frame is
  // being recorded.
  void ReloadChangedShaders() { shader_programs_.ReloadChanged(); }
  // Returns the compiler message of the most recent shader that failed to
  // reload and has not compiled since, or an empty view. Valid until the next
  // ReloadChangedShaders() call.
  [[nodiscard]] std::string_view last_shader_error() const {
    return shader_programs_.last_error();
  }

  [[nodiscard]] bool vsync() const { return vsync_; }
  [[nodiscard]] const VulkanContext& context() const { return context_; }
  [[nodiscard]] const Swapchain& swapchain() const { return swapchain_; }
  [[nodiscard]] std::optional<double> gpu_frame_ms() const {
    return gpu_timer_.last_frame_ms();
  }
  // Returns the most recent GPU time of `scope`, or nullopt if unavailable.
  [[nodiscard]] std::optional<double> gpu_scope_ms(GpuScope scope) const {
    return gpu_timer_.last_ms(scope);
  }
  // Returns the draw statistics of the most recently recorded frame.
  [[nodiscard]] RenderStats stats() const { return stats_; }

 private:
  using Cascades = std::array<lighting::Cascade, lighting::kCascadeCount>;

  // Per-frame-in-flight command recording and synchronization.
  struct Frame {
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    VkFence in_flight = VK_NULL_HANDLE;
    VkSemaphore image_acquired = VK_NULL_HANDLE;
  };

  void CreateFrames();
  // Points the passes that read render targets at the current ones.
  void BindTargets();
  [[nodiscard]] bool RebuildSwapchain();

  // Acquires the next swapchain image for `frame`. Returns nullopt, marking
  // the swapchain dirty, when it is out of date and nothing can be drawn.
  [[nodiscard]] std::optional<uint32_t> AcquireImage(const Frame& frame);
  // Submits the recorded commands of `frame` and presents `image_index`.
  void SubmitAndPresent(const Frame& frame, uint32_t image_index);
  // Records every pass of the frame described by `ctx` (see the top of this
  // file).
  void RecordCommands(const Frame& frame, const FrameContext& ctx,
                      ImDrawData* ui);
  // Computes the world-space bounds of every draw item into world_bounds_.
  void ComputeWorldBounds(const SceneView& view);

  const platform::Window& window_;
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
  DrawList draw_list_;

  ShadowPass shadow_pass_;
  DepthPrepass depth_prepass_;
  SsaoPass ssao_pass_;
  MeshPass mesh_pass_;
  SkyboxPass skybox_pass_;
  ReticlePass reticle_pass_;
  BloomPass bloom_pass_;
  TonemapPass tonemap_pass_;
  PresentPass present_pass_;

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

#endif  // BLUE_RENDER_RENDERER_H_
