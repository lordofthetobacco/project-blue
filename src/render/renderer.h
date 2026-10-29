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
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include "asset/mesh_data.h"
#include "platform/window.h"
#include "render/buffer.h"
#include "render/frame_config.h"
#include "render/gpu_timer.h"
#include "render/image.h"
#include "render/mesh.h"
#include "render/pipeline.h"
#include "render/swapchain.h"
#include "render/vulkan_context.h"

namespace blue::render {

struct Camera {
  glm::mat4 view{1.0f};
  glm::mat4 projection{1.0f};
  glm::vec3 position{0.0f};
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
};

struct RenderStats {
  uint32_t draw_calls = 0;
  uint64_t triangles = 0;
};

// Everything the renderer needs to draw one frame. `draws` is borrowed and
// only needs to stay valid for the Renderer::DrawFrame call.
struct SceneView {
  Camera camera;
  PointLight light;
  glm::vec3 ambient{0.03f};
  std::span<const DrawItem> draws;
};

// Draws scenes with a forward mesh pipeline and presents them to a window.
// Borrows `window`, which must outlive the renderer. Startup failures are
// fatal. Neither copyable nor movable. Not thread-safe.
class Renderer {
 public:
  static constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;

  // Loads the compiled shaders from `shader_dir`.
  Renderer(const platform::Window& window, const std::string& shader_dir);
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

  [[nodiscard]] bool vsync() const { return vsync_; }
  [[nodiscard]] const VulkanContext& context() const { return context_; }
  [[nodiscard]] const Swapchain& swapchain() const { return swapchain_; }
  [[nodiscard]] std::optional<double> gpu_frame_ms() const {
    return gpu_timer_.last_frame_ms();
  }
  // Returns the draw statistics of the most recently recorded frame.
  [[nodiscard]] RenderStats stats() const { return stats_; }

 private:
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
  [[nodiscard]] bool RebuildSwapchain();

  // Acquires the next swapchain image for `frame`. Returns nullopt, marking
  // the swapchain dirty, when it is out of date and nothing can be drawn.
  [[nodiscard]] std::optional<uint32_t> AcquireImage(const Frame& frame);
  // Writes the per-frame uniform block for `view` into `frame`.
  void UpdateUniforms(const Frame& frame, const SceneView& view) const;
  // Submits the recorded commands of `frame` and presents `image_index`.
  void SubmitAndPresent(const Frame& frame, uint32_t image_index);

  void RecordCommands(const Frame& frame, uint32_t image_index,
                      const SceneView& view, ImDrawData* ui);
  // Moves the color and depth images into attachment layouts.
  void TransitionToAttachments(VkCommandBuffer cmd, uint32_t image_index) const;
  // Begins dynamic rendering into the swapchain image and depth image, and
  // sets the viewport and scissor.
  void BeginRendering(VkCommandBuffer cmd, uint32_t image_index) const;
  // Records one draw per item of `view` and updates the statistics.
  void DrawScene(VkCommandBuffer cmd, const Frame& frame,
                 const SceneView& view);

  const platform::Window& window_;
  VulkanContext context_;
  bool vsync_ = true;
  bool swapchain_dirty_ = false;
  Swapchain swapchain_;
  Image depth_;

  VkDescriptorSetLayout frame_set_layout_ = VK_NULL_HANDLE;
  VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
  std::array<Frame, kFramesInFlight> frames_;
  size_t frame_index_ = 0;

  Pipeline mesh_pipeline_;
  std::vector<Mesh> meshes_;
  RenderStats stats_;
  GpuTimer gpu_timer_;
};

}  // namespace blue::render

#endif  // BLUE_RENDER_RENDERER_H_
