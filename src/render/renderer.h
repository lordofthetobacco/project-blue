#ifndef BLUE_RENDER_RENDERER_H_
#define BLUE_RENDER_RENDERER_H_

#include <imgui.h>

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
#include "core/extent.h"
#include "material/material.h"
#include "platform/window.h"
#include "render/backend.h"
#include "render/frame_config.h"
#include "render/frame_plan.h"
#include "render/gpu_scope.h"
#include "render/memory_stats.h"
#include "render/scene_view.h"
#include "render/texture_types.h"
#include "shader/compile_mode.h"

// The renderer's public API. Each GPU API is a backend behind it
// (render/renderer_backend.h); how a frame is built is described in the
// backend's renderer (e.g. render/vulkan/vulkan_renderer.h).

namespace blue::render {

class RendererBackend;

// Most anisotropy any material sampler uses unless RendererOptions caps it;
// capped by the device limit too.
inline constexpr float kMaterialAnisotropy = 8.0f;

// How Renderer is created.
struct RendererOptions {
  // The window to present to, which must outlive the renderer. When the
  // backend that starts needs other window graphics (WindowGraphicsFor()),
  // the renderer re-creates it (platform::Window::Recreate()), so nothing
  // may hold its native handles before the renderer exists. Null: headless (no
  // surface, no swapchain, SDL's video subsystem untouched); DrawFrame()
  // renders into an offscreen output image of `headless_extent` instead.
  platform::Window* window = nullptr;
  // The backend to start; another compiled one is used when it has no
  // suitable device (see Renderer()).
  Backend backend = DefaultBackend();
  // Where the build put the compiled shaders: `<shader_dir>/spirv` (Vulkan)
  // and `<shader_dir>/dxil` (Direct3D 12), see cmake/Shaders.cmake. (Metal:
  // its shader sources.)
  std::string shader_dir;
  // The Slang sources (the repository's shaders/), watched and recompiled
  // with `shader_mode` when they change (ReloadChangedShaders()). Empty, or
  // a build without BLUE_SHADER_HOT_RELOAD: no hot reload.
  std::string shader_source_dir;
  shader::CompileMode shader_mode = shader::CompileMode::kRelease;
  AdapterPreference adapter = AdapterPreference::kHighPerformance;
  // The output image's size when headless (each side at least 1).
  core::Extent2D headless_extent{1280, 720};
  // Vulkan validation layers / D3D12 debug layer with GPU-based validation;
  // Debug builds enable the API's validation regardless.
  bool gpu_validation = false;
  // Caps the material sampler's anisotropy; 1 turns anisotropic filtering
  // off (render tests: it legitimately differs between vendors).
  float max_anisotropy = kMaterialAnisotropy;
};

// The editor viewport's offscreen image (Renderer::PrepareViewport()): the
// tonemapped scene, 8-bit RGBA with the same sRGB encoding as the window
// output, ready to be shown by the UI.
struct ViewportImage {
  // What ImGui::Image() takes; 0 before the first PrepareViewport().
  ImTextureID texture = 0;
  core::Extent2D extent{};
  // Changes whenever the image is recreated (0: none yet).
  uint64_t generation = 0;
};

// A copy of the editor viewport image or the window output read back to the
// CPU (Renderer::RequestViewportReadback(), RequestOutputReadback()).
struct ViewportPixels {
  uint32_t width = 0;
  uint32_t height = 0;
  // width * height * 4 bytes, R8G8B8A8, top row first; the colour values
  // are sRGB-encoded (what the UI shows).
  std::vector<std::byte> rgba;
};

// What the GPU API's validation reported (Renderer::validation_stats()):
// the Vulkan validation layer, the D3D12 debug layer.
struct ValidationStats {
  bool enabled = false;  // Validation is running (Debug builds, or
                         // RendererOptions::gpu_validation when installed).
  uint32_t errors = 0;   // Messages so far, by severity.
  uint32_t warnings = 0;
};

// Where a viewport readback is (Renderer::TakeViewportReadback()).
enum class ReadbackStatus {
  kPending,  // Not rendered yet, or the GPU has not finished that frame.
  kReady,    // The pixels were returned (only once per ticket).
  kUnknown,  // No such ticket (never requested, or already taken).
};

// Draws scenes with a forward PBR pipeline into an HDR target, tonemaps and
// presents them to a window (or, headless, into an offscreen output image).
// Owns meshes, materials and textures. Borrows the window, which must
// outlive the renderer. Startup failures are fatal. Neither copyable nor
// movable. Not thread-safe.
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

  // Starts options.backend; when it finds no suitable device, the other
  // compiled backends are tried (FallbackOrder(): default, Vulkan, D3D12,
  // Metal), options.window re-created for each as needed, and none is fatal.
  // Loads the compiled shaders of options.shader_dir (a missing one is fatal)
  // and watches options.shader_source_dir for changes (see
  // ReloadChangedShaders()).
  explicit Renderer(const RendererOptions& options);
  // Same, with the default backend presenting to `window`.
  Renderer(platform::Window& window, const std::string& shader_dir,
           shader::CompileMode shader_mode);
  ~Renderer();

  Renderer(const Renderer&) = delete;
  Renderer& operator=(const Renderer&) = delete;

  // The backend the renderer runs on, and what it can do.
  [[nodiscard]] Backend backend() const;
  [[nodiscard]] BackendCaps caps() const;

  // Renders and presents one frame (headless: renders it into the output
  // image). `ui` is drawn on top when non-null.
  void DrawFrame(const SceneView& view, ImDrawData* ui);

  // Editor output. Makes the scene render into an offscreen image of `size`
  // pixels (each at least 1) and returns it. Call once per frame before
  // building the UI that shows it (ui::ImGuiLayer::ViewportTexture()), with
  // the size the viewport panel had last frame. On a size change the image
  // and the render targets are recreated (waiting for the frames in flight,
  // not for the whole device), at most every kViewportResizeFrames frames;
  // the returned image keeps its old extent until then. The textures of
  // earlier generations stay valid for the frames in flight only: the UI
  // must not use them after the call.
  [[nodiscard]] ViewportImage PrepareViewport(core::Extent2D size);
  // Returns the image of the last PrepareViewport() (empty before).
  [[nodiscard]] const ViewportImage& viewport() const;
  // Renders `view` into the viewport image, then `ui` (when non-null; it
  // usually shows that image) over a cleared window, and presents. Requires
  // a PrepareViewport() call before.
  void DrawViewportFrame(const SceneView& view, ImDrawData* ui);
  // Asks for a copy of the viewport image as the next DrawViewportFrame()
  // renders it (scene and overlays, no UI) and returns a ticket (never 0).
  // The copy is recorded into that frame and read once the GPU has finished
  // it, a few frames later: poll TakeViewportReadback() once per frame. No
  // device wait; the CPU-visible buffer lives only until the pixels are
  // taken. A skipped frame (minimized window) delays the copy.
  [[nodiscard]] uint64_t RequestViewportReadback();
  // Like RequestViewportReadback(), but copies the final window output of
  // the next DrawFrame() / DrawViewportFrame() (scene, overlays and UI): the
  // swapchain image, or the output image when headless. The pixels are
  // R8G8B8A8 whatever the swapchain's channel order.
  [[nodiscard]] uint64_t RequestOutputReadback();
  // Moves the pixels of `ticket` (from either request) into `pixels` when
  // they are ready (kReady); otherwise leaves `pixels` alone. Does not wait:
  // after WaitIdle() a recorded copy is ready without drawing more frames.
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
  void ReleaseMaterial(MaterialId id);
  // Destroys every released mesh and texture now instead of after the
  // frames in flight: waits for those frames' command buffers (not for the
  // whole device). For loads that replace large resources, between releasing
  // the old ones and uploading the new ones, so both never coexist in GPU
  // memory; not meant for every frame.
  void FlushReleases();
  // Returns uploaded texture `id` as an ImGui texture (the white default when
  // `id` is unknown or released). Valid until ReleaseTextures() of that id has
  // taken effect (kFramesInFlight frames after the call, or the next
  // FlushReleases()).
  [[nodiscard]] ImTextureID ui_texture(TextureId id) const;

  // Creates an untextured material (every slot its default; TextureRefs
  // in `desc` are ignored) and returns its handle.
  [[nodiscard]] MaterialId CreateMaterial(const material::MaterialDesc& desc);

  // Uploads already-encoded textures with ready mip chains (cooked prefab
  // textures: BC7 / BC5 / RGBA8) in one batch, as sampled textures: no
  // decoding and no mip generation. Returns their ids in the order of
  // `textures`. Blocks until the copies finished; logs the count, bytes and
  // time. A malformed description (no mips, wrong mip size or count) is
  // fatal: validate file data before.
  [[nodiscard]] std::vector<TextureId> UploadTextures(
      std::span<const TextureUpload> textures);
  // Creates a material whose texture slots are the uploaded `textures`; the
  // TextureRef paths of `desc` are ignored. Update(id, desc) keeps them.
  [[nodiscard]] MaterialId CreateMaterial(const material::MaterialDesc& desc,
                                          const MaterialTextures& textures);
  // Replaces material `id` by `desc` from the next frame on (live editing);
  // frames in flight keep the old version. Call between frames.
  void UpdateMaterial(MaterialId id, const material::MaterialDesc& desc);
  // Returns the current description of `id`, which must be valid.
  [[nodiscard]] const material::MaterialDesc& material(MaterialId id) const;
  [[nodiscard]] size_t material_count() const;
  [[nodiscard]] TextureStats texture_stats() const;
  // Returns what the renderer holds in GPU memory, for the device and per
  // owner; cheap enough to call every frame (see MemoryStats).
  [[nodiscard]] MemoryStats memory_stats() const;

  // Replaces the environment (sky and image-based lighting) by the
  // equirectangular HDR image `equirect`, scaled by `intensity` and turned
  // by `rotation_radians` about +Y. Waits for the GPU and filters the image:
  // meant for scene loads, not every frame.
  void SetEnvironment(const asset::ImageDataF& equirect, float intensity,
                      float rotation_radians);
  // Restores the built-in neutral default sky (intensity 1, no rotation).
  // Waits for the GPU like SetEnvironment().
  void ResetEnvironment();
  // Changes the intensity and rotation of the current environment; cheap.
  void SetEnvironmentParams(float intensity, float rotation_radians);

  // Rebuilds the swapchain before the next frame, e.g. after a resize
  // (ignored when headless).
  void RequestSwapchainRebuild();
  void SetVsync(bool enabled);

  // Recompiles the shaders whose sources changed and swaps in the new
  // pipelines; a shader that fails to compile keeps its previous version.
  // Cheap when nothing changed, but not free (it stats the watched files):
  // call it a few times per second, not every frame, and not while a frame is
  // being recorded.
  void ReloadChangedShaders();
  // Returns the compiler message of the most recent shader that failed to
  // reload and has not compiled since, or an empty view. Valid until the next
  // ReloadChangedShaders() call.
  [[nodiscard]] std::string_view last_shader_error() const;

  [[nodiscard]] bool vsync() const;
  // The name of the GPU the renderer runs on.
  [[nodiscard]] std::string_view device_name() const;
  // The size in pixels of the window output (the swapchain extent, or the
  // output image's when headless).
  [[nodiscard]] core::Extent2D output_extent() const;
  // The size the scene is rendered at, and the size it is shown at after
  // the temporal scaler (the window's or the editor viewport's): equal
  // unless an upscaling mode is active.
  [[nodiscard]] core::Extent2D render_extent() const;
  [[nodiscard]] core::Extent2D scene_output_extent() const;
  // The upscaling mode of the most recently drawn frame:
  // SceneView::post.upscaling, or kOff when the device has no temporal
  // scaler.
  [[nodiscard]] Upscaling active_upscaling() const;
  // Blocks until the GPU has finished all submitted work.
  void WaitIdle() const;

  [[nodiscard]] std::optional<double> gpu_frame_ms() const;
  // Returns the most recent GPU time of `scope`, or nullopt if unavailable.
  [[nodiscard]] std::optional<double> gpu_scope_ms(GpuScope scope) const;
  // Returns the draw statistics of the most recently recorded frame.
  [[nodiscard]] RenderStats stats() const;
  // Returns what the GPU API's validation reported so far (render tests
  // require zero errors).
  [[nodiscard]] ValidationStats validation_stats() const;

  // Called by ui::ImGuiLayer only: starts / stops ImGui's renderer backend.
  // InitImGuiRenderer() needs a current ImGui context whose platform backend is
  // already initialised. ShutdownImGuiRenderer() destroys the backend's own
  // objects (font atlas, pipelines); call it once the GPU is idle
  // (WaitIdle()).
  void InitImGuiRenderer() const;
  void ShutdownImGuiRenderer() const;

 private:
  std::unique_ptr<RendererBackend> backend_;
};

}  // namespace blue::render

#endif  // BLUE_RENDER_RENDERER_H_
