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
#include "render/buffer.h"
#include "render/draw_list.h"
#include "render/environment.h"
#include "render/frame_config.h"
#include "render/frame_globals.h"
#include "render/gpu_timer.h"
#include "render/image.h"
#include "render/material_system.h"
#include "render/memory_stats.h"
#include "render/mesh.h"
#include "render/passes/bloom_pass.h"
#include "render/passes/depth_prepass.h"
#include "render/passes/frame_context.h"
#include "render/passes/mesh_pass.h"
#include "render/passes/motion_blur_pass.h"
#include "render/passes/overlay_pass.h"
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
//   motion blur          MotionBlurPass    weapon items -> velocity -> tile
//                                          max; hdr blurred in place (via a
//                                          copy); then ResumeScenePass ->
//                                          ReticlePass -> EndScenePass. Only
//                                          in frames with weapon motion
//                                          blur, where the reticle moves
//                                          here from the scene pass.
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
//   2. Add a GpuScope (render/gpu_timer.h) and a line in RecordCommands().
//   3. Add the sources to CMakeLists.txt. Window-sized images belong in
//      RenderTargets; scene-wide inputs (environment cube, BRDF LUT, AO) are
//      already bindings of FrameGlobals' set 0 (render/frame_globals.h).
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

namespace blue::render {

// The editor viewport's offscreen image (Renderer::PrepareViewport()): the
// tonemapped scene, R8G8B8A8 (_SRGB when the swapchain is), in
// SHADER_READ_ONLY_OPTIMAL whenever the UI pass samples it.
struct ViewportImage {
  VkImageView view = VK_NULL_HANDLE;
  VkExtent2D extent{};
  // Changes whenever the image is recreated (0: none yet); UI code
  // registers the view again when it changes.
  uint64_t generation = 0;
};

// A copy of the editor viewport image read back to the CPU
// (Renderer::RequestViewportReadback()).
struct ViewportPixels {
  uint32_t width = 0;
  uint32_t height = 0;
  // width * height * 4 bytes, R8G8B8A8, top row first; the colour values
  // are sRGB-encoded (what the UI shows).
  std::vector<std::byte> rgba;
};

// Where a viewport readback is (Renderer::TakeViewportReadback()).
enum class ReadbackStatus {
  kPending,  // Not rendered yet, or the GPU has not finished that frame.
  kReady,    // The pixels were returned (only once per ticket).
  kUnknown,  // No such ticket (never requested, or already taken).
};

// Draws scenes with a forward PBR pipeline into an HDR target, tonemaps and
// presents them to a window. Owns meshes, materials and textures. Borrows
// `window`, which must outlive the renderer. Startup failures are fatal.
// Neither copyable nor movable. Not thread-safe.
//
// Two outputs: DrawFrame() presents the scene to the window (the game);
// PrepareViewport() + DrawViewportFrame() render it into an offscreen image
// that the UI shows in a panel, and the UI alone to the window (the editor).
// Once PrepareViewport() was called the render targets follow the viewport
// size instead of the window's.
class Renderer {
 public:
  // A viewport size change recreates the image and render targets at most
  // once per this many frames; in between the last image is shown
  // stretched, so dragging a panel edge does not stall every frame.
  static constexpr uint64_t kViewportResizeFrames = 4;

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

  // Editor output. Makes the scene render into an offscreen image of `size`
  // pixels (each at least 1) and returns it. Call once per frame before
  // building the UI that shows it (ui::ImGuiLayer::ViewportTexture()), with
  // the size the viewport panel had last frame. On a size change the image
  // and the render targets are recreated (waiting for the frames in flight,
  // not for the whole device), at most every kViewportResizeFrames frames;
  // the returned image keeps its old extent until then. Images of earlier
  // generations are destroyed here: the UI must not use their views after
  // the call.
  [[nodiscard]] ViewportImage PrepareViewport(VkExtent2D size);
  // Returns the image of the last PrepareViewport() (empty before).
  [[nodiscard]] const ViewportImage& viewport() const { return viewport_; }
  // Renders `view` into the viewport image, then `ui` (when non-null; it
  // usually shows that image) over a cleared window, and presents. Requires
  // a PrepareViewport() call before.
  void DrawViewportFrame(const SceneView& view, ImDrawData* ui);
  // Asks for a copy of the viewport image as the next DrawViewportFrame()
  // renders it (scene and overlays, no UI) and returns a ticket (never 0).
  // The copy is recorded into that frame and read once the GPU has finished
  // it, a few frames later: poll TakeViewportReadback() once per frame. No
  // device wait; the host-visible buffer lives only until the pixels are
  // taken. A skipped frame (minimized window) delays the copy.
  [[nodiscard]] uint64_t RequestViewportReadback();
  // Moves the pixels of `ticket` into `pixels` when they are ready
  // (kReady); otherwise leaves `pixels` alone.
  [[nodiscard]] ReadbackStatus TakeViewportReadback(uint64_t ticket,
                                                    ViewportPixels& pixels);

  // Uploads `data` and returns its handle. Meshes stay resident until
  // ReleaseMesh().
  [[nodiscard]] MeshId UploadMesh(const asset::MeshData& data) {
    return UploadMesh(data.vertices, data.indices);
  }
  // Same, from borrowed vertex and index arrays (e.g. the bulk data of a
  // loaded prefab), without an intermediate copy.
  [[nodiscard]] MeshId UploadMesh(std::span<const asset::Vertex> vertices,
                                  std::span<const asset::Index> indices);

  // Releasing GPU resources. Each call takes effect at once for new frames
  // and destroys the GPU objects only once no frame in flight can use them.
  // Ids are never reused: a released slot stays a tombstone, so a stale id
  // cannot alias a newer resource. Unknown or already released ids are
  // ignored. Rules for stale ids in a SceneView: a DrawItem whose mesh was
  // released is skipped (not drawn, no error); a released material draws as
  // kDefaultMaterial. Releasing a texture that a live material still
  // samples is a caller error (release the materials first). Call between
  // frames.
  void ReleaseMesh(MeshId id);
  void ReleaseTextures(std::span<const TextureId> ids);
  void ReleaseMaterial(MaterialId id) { materials_.Release(id); }
  // Destroys every released mesh and texture now instead of after the
  // frames in flight: waits for those frames' fences (not for the whole
  // device). For loads that replace large resources, between releasing the
  // old ones and uploading the new ones, so both never coexist in device
  // memory; not meant for every frame.
  void FlushReleases();
  // Returns the view of uploaded texture `id` (the white default when it is
  // unknown or released), e.g. to show it in the UI.
  [[nodiscard]] VkImageView texture_view(TextureId id) const {
    return textures_.view(id);
  }

  // Creates an untextured material (every slot its default; TextureRefs
  // in `desc` are ignored) and returns its handle.
  [[nodiscard]] MaterialId CreateMaterial(const material::MaterialDesc& desc) {
    return materials_.Create(desc);
  }

  // Uploads already-encoded textures with ready mip chains (cooked prefab
  // textures: BC7 / BC5 / RGBA8) in one batch; see TextureCache::Upload().
  [[nodiscard]] std::vector<TextureId> UploadTextures(
      std::span<const TextureUpload> textures) {
    return textures_.Upload(textures);
  }
  // Creates a material whose texture slots are the uploaded `textures`; the
  // TextureRef paths of `desc` are ignored. Update(id, desc) keeps them.
  [[nodiscard]] MaterialId CreateMaterial(const material::MaterialDesc& desc,
                                          const MaterialTextures& textures) {
    return materials_.Create(desc, textures);
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
  // Returns what the renderer holds in device memory, per heap and per
  // owner; cheap enough to call every frame (see MemoryStats).
  [[nodiscard]] MemoryStats memory_stats() const;

  // Replaces the environment (sky and image-based lighting) by the
  // equirectangular HDR image `equirect`, scaled by `intensity` and turned
  // by `rotation_radians` about +Y. Waits for the GPU and filters the image
  // (see render::Environment): meant for scene loads, not every frame.
  void SetEnvironment(const asset::ImageDataF& equirect, float intensity,
                      float rotation_radians) {
    context_.WaitIdle();
    environment_.Set(equirect, intensity, rotation_radians);
  }
  // Restores the built-in neutral default sky (intensity 1, no rotation).
  // Waits for the GPU like SetEnvironment().
  void ResetEnvironment() {
    context_.WaitIdle();
    environment_.Set({}, 1.0f, 0.0f);
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

  // Where a frame's tonemapped scene goes.
  enum class Output {
    kSwapchain,  // The window (DrawFrame()).
    kViewport,   // viewport_image_, with the UI alone on the window.
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
  DrawList draw_list_;

  ShadowPass shadow_pass_;
  DepthPrepass depth_prepass_;
  SsaoPass ssao_pass_;
  MeshPass mesh_pass_;
  SkyboxPass skybox_pass_;
  ReticlePass reticle_pass_;
  MotionBlurPass motion_blur_pass_;
  BloomPass bloom_pass_;
  TonemapPass tonemap_pass_;
  OverlayPass overlay_pass_;
  PresentPass present_pass_;
  // The present pass into viewport_image_'s format; created by the first
  // PrepareViewport().
  std::optional<PresentPass> viewport_present_pass_;

  // Editor viewport output (PrepareViewport()); empty in the game.
  Image viewport_image_;
  ViewportImage viewport_;
  // frame_serial_ at the last viewport recreation.
  uint64_t viewport_resized_at_ = 0;
  // Requested viewport copies (RequestViewportReadback()).
  struct Readback {
    uint64_t ticket = 0;
    // The frame that records the copy; 0 until recorded.
    uint64_t frame_serial = 0;
    VkExtent2D extent{};
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

}  // namespace blue::render

#endif  // BLUE_RENDER_RENDERER_H_
