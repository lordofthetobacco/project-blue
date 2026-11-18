#ifndef BLUE_RENDER_PIPELINE_H_
#define BLUE_RENDER_PIPELINE_H_

#include <vulkan/vulkan.h>

#include <cstdint>
#include <span>

namespace blue::render {

// How the fragment color is combined with the color attachment.
enum class BlendMode {
  kReplace,   // Overwrites the attachment.
  kAdditive,  // src + dst; alpha unchanged.
  kAlpha,     // src * src.a + dst * (1 - src.a); alpha unchanged.
};

// Describes a graphics pipeline. The spans are borrowed and only need to stay
// valid for the duration of the Pipeline constructor call.
struct GraphicsPipelineDesc {
  std::span<const uint32_t> vertex_spirv;
  // Empty for a pipeline without a fragment stage (depth-only).
  std::span<const uint32_t> fragment_spirv;
  // Values of the fragment shader's specialization constants: element i is
  // constant_id i, read as a 32-bit int / uint / bool (0 = false).
  std::span<const uint32_t> fragment_constants;

  std::span<const VkVertexInputBindingDescription> vertex_bindings;
  std::span<const VkVertexInputAttributeDescription> vertex_attributes;
  std::span<const VkDescriptorSetLayout> set_layouts;
  std::span<const VkPushConstantRange> push_constants;

  // VK_FORMAT_UNDEFINED means no color attachment (depth-only pipeline).
  VkFormat color_format = VK_FORMAT_UNDEFINED;
  // VK_FORMAT_UNDEFINED means no depth attachment and no depth test (e.g. a
  // full-screen pass).
  VkFormat depth_format = VK_FORMAT_UNDEFINED;
  VkCullModeFlags cull_mode = VK_CULL_MODE_BACK_BIT;
  VkFrontFace front_face = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  // EQUAL lets a pass shade only the surfaces a depth pre-pass kept.
  VkCompareOp depth_compare_op = VK_COMPARE_OP_GREATER;
  // False keeps the depth test but leaves the depth buffer untouched, e.g.
  // for overlays drawn after the opaque scene.
  bool depth_write = true;
  BlendMode blend = BlendMode::kReplace;
  // Clamps instead of clips fragments outside the near/far planes (needs the
  // depthClamp device feature).
  bool depth_clamp = false;
  // Enables depth bias; the factors are dynamic, set with vkCmdSetDepthBias.
  bool depth_bias = false;
};

// Graphics pipeline for dynamic rendering, with its layout. Viewport and
// scissor (and the depth bias, if enabled) are dynamic. Neither copyable nor
// movable. Invalid SPIR-V or a failed Vulkan call is fatal. Not thread-safe.
class Pipeline {
 public:
  Pipeline(VkDevice device, const GraphicsPipelineDesc& desc);
  ~Pipeline();

  Pipeline(const Pipeline&) = delete;
  Pipeline& operator=(const Pipeline&) = delete;

  [[nodiscard]] VkPipeline handle() const { return pipeline_; }
  [[nodiscard]] VkPipelineLayout layout() const { return layout_; }

 private:
  VkDevice device_ = VK_NULL_HANDLE;
  VkPipelineLayout layout_ = VK_NULL_HANDLE;
  VkPipeline pipeline_ = VK_NULL_HANDLE;
};

}  // namespace blue::render

#endif  // BLUE_RENDER_PIPELINE_H_
