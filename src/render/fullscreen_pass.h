#ifndef BLUE_RENDER_FULLSCREEN_PASS_H_
#define BLUE_RENDER_FULLSCREEN_PASS_H_

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "render/pipeline.h"
#include "render/shader_programs.h"
#include "render/vulkan_context.h"

namespace blue::render {

struct FullscreenPassDesc {
  // Fragment shader file; the vertex shader is shaders/fullscreen.vert.
  std::string fragment_path;
  // sampler2D inputs at bindings 0 .. input_count - 1 of set 0.
  uint32_t input_count = 1;
  // Independent sets of inputs, chosen per Record() call (e.g. one per mip
  // of a bloom chain).
  uint32_t input_set_count = 1;
  // Push constant block visible to both stages, in bytes (at most 128).
  uint32_t push_constant_size = 0;
  VkFormat output_format = VK_FORMAT_UNDEFINED;
  BlendMode blend = BlendMode::kReplace;
  // When set, bound as set 1 (FrameGlobals: FrameUniforms and the shared
  // images), for passes that need the camera or the shadow map.
  VkDescriptorSetLayout frame_set_layout = VK_NULL_HANDLE;
};

// Where a FullscreenPass draws: a color view in COLOR_ATTACHMENT_OPTIMAL.
struct FullscreenTarget {
  VkImageView view = VK_NULL_HANDLE;
  VkExtent2D extent{};
  // LOAD keeps what is there (e.g. for additive blending); DONT_CARE when
  // the triangle overwrites every pixel anyway.
  VkAttachmentLoadOp load_op = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  // The pixels to draw (render area and scissor), inside `extent`; pixels
  // outside keep their contents. An empty extent draws the whole target.
  // The viewport, hence inUv, always spans the whole target.
  VkRect2D area{};
};

// A pass that runs one fragment shader over a whole target: tonemapping,
// copies, and later bloom, SSAO and FXAA. Owns its descriptor sets and the
// pipeline (hot-reloaded through `programs`). The fragment shader sees
// inUv in [0, 1] with (0, 0) at the top-left pixel corner.
//
// Image barriers are the caller's job: the inputs must be in the layout
// given to SetInputs() and the target in COLOR_ATTACHMENT_OPTIMAL when
// the recorded commands run. Borrows `context` and `programs`, which must
// outlive it. Neither copyable nor movable. Not thread-safe.
class FullscreenPass {
 public:
  FullscreenPass(const VulkanContext& context, ShaderPrograms& programs,
                 const std::string& shader_dir, const FullscreenPassDesc& desc);
  ~FullscreenPass();

  FullscreenPass(const FullscreenPass&) = delete;
  FullscreenPass& operator=(const FullscreenPass&) = delete;

  // Points input set `set_index` at `views` (input_count of them), sampled
  // with `sampler`, in `layout` when the pass runs (e.g.
  // DEPTH_READ_ONLY_OPTIMAL for a depth image that is also a read-only
  // attachment). No frame using the set may be in flight (call while the GPU
  // is idle, e.g. after the render targets were recreated).
  void SetInputs(
      uint32_t set_index, std::span<const VkImageView> views, VkSampler sampler,
      VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

  // Records rendering into `target` with input set `set_index`. `push` must
  // be empty or exactly push_constant_size bytes. `frame_set` is required
  // when the pass was created with a frame_set_layout.
  void Record(VkCommandBuffer cmd, const FullscreenTarget& target,
              uint32_t set_index = 0, std::span<const std::byte> push = {},
              VkDescriptorSet frame_set = VK_NULL_HANDLE) const;

 private:
  VkDevice device_;
  FullscreenPassDesc desc_;
  VkDescriptorSetLayout input_layout_ = VK_NULL_HANDLE;
  VkDescriptorPool pool_ = VK_NULL_HANDLE;
  std::vector<VkDescriptorSet> sets_;
  // Replaced when its shaders are reloaded; never null after construction.
  std::unique_ptr<Pipeline> pipeline_;
};

// Returns the bytes of a push constant block, for FullscreenPass::Record().
template <typename T>
[[nodiscard]] std::span<const std::byte> PushBytes(const T& block) {
  return std::as_bytes(std::span(&block, 1));
}

}  // namespace blue::render

#endif  // BLUE_RENDER_FULLSCREEN_PASS_H_
