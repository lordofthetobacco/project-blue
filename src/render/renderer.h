#ifndef BLUE_RENDER_RENDERER_H_
#define BLUE_RENDER_RENDERER_H_

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

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include "asset/mesh_data.h"
#include "camera/frustum.h"
#include "lighting/cascades.h"
#include "platform/window.h"
#include "render/buffer.h"
#include "render/frame_config.h"
#include "render/gpu_timer.h"
#include "render/image.h"
#include "render/mesh.h"
#include "render/pipeline.h"
#include "render/shader_programs.h"
#include "render/swapchain.h"
#include "render/vulkan_context.h"
#include "scene/scene.h"
#include "shader/shader_compiler.h"

namespace blue::render {

struct Camera {
  glm::mat4 view{1.0f};
  glm::mat4 projection{1.0f};
  glm::vec3 position{0.0f};
  // Parameters of `projection`, needed to fit the shadow cascades.
  float vertical_fov_radians = 1.0f;
  float near_plane = 0.1f;
};

struct PointLight {
  glm::vec3 position{0.0f};
  glm::vec3 color{1.0f};
  float intensity = 1.0f;
};

// Handle to a mesh owned by the renderer.
enum class MeshId : uint32_t {};

struct DrawItem {
  MeshId mesh;
  glm::mat4 transform;
  // Local-space bounds of the mesh; culled against the camera and the shadow
  // cascades after `transform`.
  asset::Bounds bounds;
};

// Draw counts of the most recent frame. Culled items were submitted in the
// SceneView but skipped because their bounds fell outside the view.
struct RenderStats {
  uint32_t draw_calls = 0;  // Main pass.
  uint64_t triangles = 0;   // Main pass.
  uint32_t culled_draws = 0;
  uint32_t shadow_draw_calls = 0;  // Summed over the cascades.
  uint32_t shadow_culled_draws = 0;
};

// A collimated reflex sight, drawn over the opaque scene. The lens window is
// a disc in world space; its dot is an image at infinity along `axis`, so a
// pixel of the window shows the dot when the direction from the camera
// through it is within the dot's angular radius of `axis`. The window is
// depth tested (whatever is in front hides it) but writes no depth.
struct ReticleSight {
  // Maps the unit disc (x, y in [-1, 1], z = 0) onto the lens window in
  // world space.
  glm::mat4 lens{1.0f};
  glm::vec3 axis{0.0f, 0.0f, -1.0f};  // world, unit, pointing downrange
  // Nominal dot size: 3 MOA diameter.
  float dot_angular_radius = 4.36e-4f;      // radians
  glm::vec3 dot_color{4.0f, 0.12f, 0.06f};  // linear, intensity included
  // The dot never gets smaller than this many pixels across, so it stays
  // visible when the nominal size is below a pixel.
  float min_dot_pixels = 2.0f;
  glm::vec3 tint{0.004f, 0.006f, 0.005f};  // light added over the window
};

// Everything the renderer needs to draw one frame. `draws` is borrowed and
// only needs to stay valid for the Renderer::DrawFrame call.
struct SceneView {
  Camera camera;
  PointLight light;
  glm::vec3 ambient{0.03f};
  scene::SunLight sun;
  // Shadows are skipped entirely when this is false or `sun` casts none.
  bool shadows_enabled = true;
  // Tints each pixel by the shadow cascade it samples.
  bool cascade_debug = false;
  std::span<const DrawItem> draws;
  // Drawn after the draw items when present.
  std::optional<ReticleSight> sight;
};

// Draws scenes with a forward mesh pipeline and presents them to a window.
// Borrows `window`, which must outlive the renderer. Startup failures are
// fatal. Neither copyable nor movable. Not thread-safe.
class Renderer {
 public:
  static constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
  static constexpr VkFormat kShadowFormat = VK_FORMAT_D16_UNORM;

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

  // Per-frame-in-flight resources.
  struct Frame {
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    VkFence in_flight = VK_NULL_HANDLE;
    VkSemaphore image_acquired = VK_NULL_HANDLE;
    Buffer uniforms;
    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
  };

  void CreateFrames();
  // Registers the mesh shaders in `shader_dir` and creates the mesh pipeline.
  void AddMeshProgram(const std::string& shader_dir);
  // Registers the shadow vertex shader and creates the depth-only pipeline.
  void AddShadowProgram(const std::string& shader_dir);
  // Registers the reflex sight shaders and creates their additive pipeline.
  void AddReticleProgram(const std::string& shader_dir);
  [[nodiscard]] bool RebuildSwapchain();

  // Acquires the next swapchain image for `frame`. Returns nullopt, marking
  // the swapchain dirty, when it is out of date and nothing can be drawn.
  [[nodiscard]] std::optional<uint32_t> AcquireImage(const Frame& frame);
  // Writes the per-frame uniform block for `view` into `frame`. `cascades`
  // are only read when `shadows` is true.
  void UpdateUniforms(const Frame& frame, const SceneView& view,
                      const Cascades& cascades, bool shadows) const;
  // Submits the recorded commands of `frame` and presents `image_index`.
  void SubmitAndPresent(const Frame& frame, uint32_t image_index);

  void RecordCommands(const Frame& frame, uint32_t image_index,
                      const SceneView& view, const Cascades& cascades,
                      bool shadows, const camera::Frustum& camera_frustum,
                      ImDrawData* ui);
  // Computes the world-space bounds of every draw item into world_bounds_.
  void ComputeWorldBounds(const SceneView& view);
  // Renders the draw items that reach each cascade into its layer of the
  // shadow map and leaves the map readable by fragment shaders. Must be
  // recorded outside of a rendering pass, before the main pass.
  void RecordShadowPass(VkCommandBuffer cmd, const SceneView& view,
                        const Cascades& cascades);
  // Moves the shadow map out of its initial layout the first time, so it can
  // be bound while the shadow pass is skipped.
  void InitializeShadowMapLayout(VkCommandBuffer cmd);
  // Moves the color and depth images into attachment layouts.
  void TransitionToAttachments(VkCommandBuffer cmd, uint32_t image_index) const;
  // Begins dynamic rendering into the swapchain image and depth image, and
  // sets the viewport and scissor.
  void BeginRendering(VkCommandBuffer cmd, uint32_t image_index) const;
  // Records one draw per item of `view` inside `camera_frustum` and updates
  // the statistics.
  void DrawScene(VkCommandBuffer cmd, const Frame& frame, const SceneView& view,
                 const camera::Frustum& camera_frustum);
  // Records the reflex sight of `view`, if any. Call after DrawScene, inside
  // the same rendering pass, so the scene's depth hides the window.
  void DrawReticle(VkCommandBuffer cmd, const Frame& frame,
                   const SceneView& view);

  const platform::Window& window_;
  VulkanContext context_;
  bool vsync_ = true;
  bool swapchain_dirty_ = false;
  Swapchain swapchain_;
  Image depth_;
  // One map shared by all frames in flight: see RecordShadowPass().
  Image shadow_map_;
  VkSampler shadow_sampler_ = VK_NULL_HANDLE;
  // False until the map has been moved out of its initial layout.
  bool shadow_map_initialized_ = false;

  VkDescriptorSetLayout frame_set_layout_ = VK_NULL_HANDLE;
  VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
  std::array<Frame, kFramesInFlight> frames_;
  size_t frame_index_ = 0;

  ShaderPrograms shader_programs_;
  // Replaced when its shaders are reloaded; never null after construction.
  std::unique_ptr<Pipeline> mesh_pipeline_;
  std::unique_ptr<Pipeline> shadow_pipeline_;
  std::unique_ptr<Pipeline> reticle_pipeline_;
  std::vector<Mesh> meshes_;
  // World-space bounds of the current frame's draw items, parallel to
  // SceneView::draws; kept as a member to reuse its allocation.
  std::vector<camera::Aabb> world_bounds_;
  RenderStats stats_;
  GpuTimer gpu_timer_;
};

}  // namespace blue::render

#endif  // BLUE_RENDER_RENDERER_H_
