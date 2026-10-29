#include "render/pipeline.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "core/log.h"
#include "platform/file_system.h"
#include "render/vk_check.h"

namespace blue::render {

namespace {

constexpr size_t kMaxShaderBytes = 16 * 1024 * 1024;

// Reads the SPIR-V binary at `path`. Fatal if it is missing or malformed.
std::vector<uint32_t> ReadSpirv(const std::string& path) {
  const auto file = platform::ReadFile(path, kMaxShaderBytes);
  if (!file.ok()) {
    core::log::Fatal("Cannot read shader {}: {}", path,
                     platform::ToString(file.error()));
  }

  const std::vector<std::byte>& bytes = file.value();
  if (bytes.empty() || bytes.size() % sizeof(uint32_t) != 0) {
    core::log::Fatal("Shader {} is not valid SPIR-V ({} bytes)", path,
                     bytes.size());
  }

  std::vector<uint32_t> code(bytes.size() / sizeof(uint32_t));
  std::memcpy(code.data(), bytes.data(), bytes.size());
  return code;
}

VkShaderModule CreateShaderModule(VkDevice device, const std::string& path) {
  const std::vector<uint32_t> code = ReadSpirv(path);
  const VkShaderModuleCreateInfo create_info{
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = code.size() * sizeof(uint32_t),
      .pCode = code.data(),
  };
  VkShaderModule module = VK_NULL_HANDLE;
  CheckVk(vkCreateShaderModule(device, &create_info, nullptr, &module));
  return module;
}

}  // namespace

Pipeline::Pipeline(VkDevice device, const GraphicsPipelineDesc& desc)
    : device_(device) {
  const VkPipelineLayoutCreateInfo layout_info{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = static_cast<uint32_t>(desc.set_layouts.size()),
      .pSetLayouts = desc.set_layouts.data(),
      .pushConstantRangeCount =
          static_cast<uint32_t>(desc.push_constants.size()),
      .pPushConstantRanges = desc.push_constants.data(),
  };
  CheckVk(vkCreatePipelineLayout(device_, &layout_info, nullptr, &layout_));

  const VkShaderModule vertex_module =
      CreateShaderModule(device_, desc.vertex_shader);
  const VkShaderModule fragment_module =
      CreateShaderModule(device_, desc.fragment_shader);

  const std::array stages = {
      VkPipelineShaderStageCreateInfo{
          .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT,
          .module = vertex_module,
          .pName = "main",
      },
      VkPipelineShaderStageCreateInfo{
          .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
          .module = fragment_module,
          .pName = "main",
      },
  };

  const VkPipelineVertexInputStateCreateInfo vertex_input{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
      .vertexBindingDescriptionCount =
          static_cast<uint32_t>(desc.vertex_bindings.size()),
      .pVertexBindingDescriptions = desc.vertex_bindings.data(),
      .vertexAttributeDescriptionCount =
          static_cast<uint32_t>(desc.vertex_attributes.size()),
      .pVertexAttributeDescriptions = desc.vertex_attributes.data(),
  };
  const VkPipelineInputAssemblyStateCreateInfo input_assembly{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
  };
  const VkPipelineViewportStateCreateInfo viewport{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
      .viewportCount = 1,
      .scissorCount = 1,
  };
  const VkPipelineRasterizationStateCreateInfo rasterization{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .polygonMode = VK_POLYGON_MODE_FILL,
      .cullMode = desc.cull_mode,
      .frontFace = desc.front_face,
      .lineWidth = 1.0f,
  };
  const VkPipelineMultisampleStateCreateInfo multisample{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
  };
  const bool has_depth = desc.depth_format != VK_FORMAT_UNDEFINED;
  const VkPipelineDepthStencilStateCreateInfo depth_stencil{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
      .depthTestEnable = has_depth,
      .depthWriteEnable = has_depth,
      .depthCompareOp = desc.depth_compare_op,
  };
  const VkPipelineColorBlendAttachmentState blend_attachment{
      .blendEnable = VK_FALSE,
      .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
  };
  const VkPipelineColorBlendStateCreateInfo color_blend{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = 1,
      .pAttachments = &blend_attachment,
  };
  constexpr std::array dynamic_states = {VK_DYNAMIC_STATE_VIEWPORT,
                                         VK_DYNAMIC_STATE_SCISSOR};
  const VkPipelineDynamicStateCreateInfo dynamic_state{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
      .dynamicStateCount = static_cast<uint32_t>(dynamic_states.size()),
      .pDynamicStates = dynamic_states.data(),
  };
  const VkPipelineRenderingCreateInfo rendering_info{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
      .colorAttachmentCount = 1,
      .pColorAttachmentFormats = &desc.color_format,
      .depthAttachmentFormat = desc.depth_format,
  };

  const VkGraphicsPipelineCreateInfo create_info{
      .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .pNext = &rendering_info,
      .stageCount = static_cast<uint32_t>(stages.size()),
      .pStages = stages.data(),
      .pVertexInputState = &vertex_input,
      .pInputAssemblyState = &input_assembly,
      .pViewportState = &viewport,
      .pRasterizationState = &rasterization,
      .pMultisampleState = &multisample,
      .pDepthStencilState = &depth_stencil,
      .pColorBlendState = &color_blend,
      .pDynamicState = &dynamic_state,
      .layout = layout_,
  };
  CheckVk(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &create_info,
                                    nullptr, &pipeline_));

  vkDestroyShaderModule(device_, fragment_module, nullptr);
  vkDestroyShaderModule(device_, vertex_module, nullptr);
}

Pipeline::~Pipeline() {
  vkDestroyPipeline(device_, pipeline_, nullptr);
  vkDestroyPipelineLayout(device_, layout_, nullptr);
}

}  // namespace blue::render
