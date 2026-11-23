#include "render/vulkan/pipeline.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "core/log.h"
#include "render/vulkan/vk_check.h"

namespace blue::render::vulkan {

namespace {

// Returns the blend state of `mode` for the single color attachment.
VkPipelineColorBlendAttachmentState BlendAttachment(BlendMode mode) {
  VkPipelineColorBlendAttachmentState state{
      .blendEnable = mode == BlendMode::kReplace ? VK_FALSE : VK_TRUE,
      .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
      .dstColorBlendFactor = VK_BLEND_FACTOR_ONE,
      .colorBlendOp = VK_BLEND_OP_ADD,
      .srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
      .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
      .alphaBlendOp = VK_BLEND_OP_ADD,
      .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
  };
  if (mode == BlendMode::kAlpha) {
    state.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    state.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  }
  return state;
}

VkShaderModule CreateShaderModule(VkDevice device,
                                  std::span<const uint32_t> code) {
  if (code.empty()) {
    core::log::Fatal("Cannot create a pipeline from empty SPIR-V");
  }
  const VkShaderModuleCreateInfo create_info{
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = code.size_bytes(),
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
      CreateShaderModule(device_, desc.vertex_spirv);
  VkShaderModule fragment_module = VK_NULL_HANDLE;

  std::vector<VkPipelineShaderStageCreateInfo> stages;
  stages.push_back({
      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .stage = VK_SHADER_STAGE_VERTEX_BIT,
      .module = vertex_module,
      .pName = "main",
  });
  std::vector<VkSpecializationMapEntry> constant_entries;
  for (size_t i = 0; i < desc.fragment_constants.size(); ++i) {
    constant_entries.push_back({
        .constantID = static_cast<uint32_t>(i),
        .offset = static_cast<uint32_t>(i * sizeof(uint32_t)),
        .size = sizeof(uint32_t),
    });
  }
  const VkSpecializationInfo specialization{
      .mapEntryCount = static_cast<uint32_t>(constant_entries.size()),
      .pMapEntries = constant_entries.data(),
      .dataSize = desc.fragment_constants.size_bytes(),
      .pData = desc.fragment_constants.data(),
  };
  if (!desc.fragment_spirv.empty()) {
    fragment_module = CreateShaderModule(device_, desc.fragment_spirv);
    stages.push_back({
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
        .module = fragment_module,
        .pName = "main",
        .pSpecializationInfo =
            constant_entries.empty() ? nullptr : &specialization,
    });
  }

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
      .topology = desc.topology,
  };
  const VkPipelineViewportStateCreateInfo viewport{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
      .viewportCount = 1,
      .scissorCount = 1,
  };
  const VkPipelineRasterizationStateCreateInfo rasterization{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .depthClampEnable = desc.depth_clamp,
      .polygonMode = VK_POLYGON_MODE_FILL,
      .cullMode = desc.cull_mode,
      .frontFace = desc.front_face,
      .depthBiasEnable = desc.depth_bias,
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
      .depthWriteEnable = has_depth && desc.depth_write,
      .depthCompareOp = desc.depth_compare_op,
  };
  const VkPipelineColorBlendAttachmentState blend_attachment =
      BlendAttachment(desc.blend);
  const uint32_t color_attachment_count =
      desc.color_format != VK_FORMAT_UNDEFINED ? 1 : 0;
  const VkPipelineColorBlendStateCreateInfo color_blend{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = color_attachment_count,
      .pAttachments = &blend_attachment,
  };
  std::vector<VkDynamicState> dynamic_states = {VK_DYNAMIC_STATE_VIEWPORT,
                                                VK_DYNAMIC_STATE_SCISSOR};
  if (desc.depth_bias) {
    dynamic_states.push_back(VK_DYNAMIC_STATE_DEPTH_BIAS);
  }
  const VkPipelineDynamicStateCreateInfo dynamic_state{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
      .dynamicStateCount = static_cast<uint32_t>(dynamic_states.size()),
      .pDynamicStates = dynamic_states.data(),
  };
  const VkPipelineRenderingCreateInfo rendering_info{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
      .colorAttachmentCount = color_attachment_count,
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

  if (fragment_module != VK_NULL_HANDLE) {
    vkDestroyShaderModule(device_, fragment_module, nullptr);
  }
  vkDestroyShaderModule(device_, vertex_module, nullptr);
}

Pipeline::~Pipeline() {
  vkDestroyPipeline(device_, pipeline_, nullptr);
  vkDestroyPipelineLayout(device_, layout_, nullptr);
}

}  // namespace blue::render::vulkan
