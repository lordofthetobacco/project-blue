#ifndef BLUE_RENDER_PIPELINE_H_
#define BLUE_RENDER_PIPELINE_H_

#include <vulkan/vulkan.h>

#include <span>
#include <string>

namespace blue::render {

// Describes a graphics pipeline. The spans are borrowed and only need to stay
// valid for the duration of the Pipeline constructor call.
struct GraphicsPipelineDesc {
  std::string vertex_shader;    // Path to the compiled SPIR-V.
  std::string fragment_shader;  // Path to the compiled SPIR-V.

  std::span<const VkVertexInputBindingDescription> vertex_bindings;
  std::span<const VkVertexInputAttributeDescription> vertex_attributes;
  std::span<const VkDescriptorSetLayout> set_layouts;
  std::span<const VkPushConstantRange> push_constants;

  VkFormat color_format = VK_FORMAT_UNDEFINED;
  VkFormat depth_format = VK_FORMAT_UNDEFINED;
  VkCullModeFlags cull_mode = VK_CULL_MODE_BACK_BIT;
  VkFrontFace front_face = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  VkCompareOp depth_compare_op = VK_COMPARE_OP_GREATER;
};

// Graphics pipeline for dynamic rendering, with its layout. Viewport and
// scissor are dynamic. Neither copyable nor movable. A missing or invalid
// shader is fatal. Not thread-safe.
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
