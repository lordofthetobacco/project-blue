#include "render/fullscreen_pass.h"

#include <array>
#include <vector>

#include "core/log.h"
#include "core/path.h"
#include "render/vk_check.h"

namespace blue::render {

namespace {

// The vertex shader emits one triangle that covers the whole viewport.
constexpr uint32_t kFullscreenVertexCount = 3;
constexpr uint32_t kMaxPushConstantSize = 128;

constexpr VkShaderStageFlags kPushStages =
    VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

VkDescriptorSetLayout CreateInputLayout(VkDevice device, uint32_t count) {
  std::vector<VkDescriptorSetLayoutBinding> bindings(count);
  for (uint32_t i = 0; i < count; ++i) {
    bindings[i] = {
        .binding = i,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
    };
  }
  const VkDescriptorSetLayoutCreateInfo create_info{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = count,
      .pBindings = bindings.data(),
  };
  VkDescriptorSetLayout layout = VK_NULL_HANDLE;
  CheckVk(vkCreateDescriptorSetLayout(device, &create_info, nullptr, &layout));
  return layout;
}

}  // namespace

FullscreenPass::FullscreenPass(const VulkanContext& context,
                               ShaderPrograms& programs,
                               const std::string& shader_dir,
                               const FullscreenPassDesc& desc)
    : device_(context.device()), desc_(desc) {
  if (desc.input_count == 0 || desc.input_set_count == 0 ||
      desc.push_constant_size > kMaxPushConstantSize) {
    core::log::Fatal("Invalid full-screen pass '{}'", desc.fragment_path);
  }
  input_layout_ = CreateInputLayout(device_, desc.input_count);

  const VkDescriptorPoolSize pool_size{
      .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      .descriptorCount = desc.input_count * desc.input_set_count,
  };
  const VkDescriptorPoolCreateInfo pool_info{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = desc.input_set_count,
      .poolSizeCount = 1,
      .pPoolSizes = &pool_size,
  };
  CheckVk(vkCreateDescriptorPool(device_, &pool_info, nullptr, &pool_));
  sets_.resize(desc.input_set_count);
  const std::vector<VkDescriptorSetLayout> layouts(desc.input_set_count,
                                                   input_layout_);
  const VkDescriptorSetAllocateInfo set_info{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = pool_,
      .descriptorSetCount = desc.input_set_count,
      .pSetLayouts = layouts.data(),
  };
  CheckVk(vkAllocateDescriptorSets(device_, &set_info, sets_.data()));

  programs.Add(
      core::JoinPath(shader_dir, "fullscreen.vert"), desc.fragment_path,
      [this](std::span<const uint32_t> vertex,
             std::span<const uint32_t> fragment) {
        const std::array<VkDescriptorSetLayout, 2> set_layouts{
            input_layout_, desc_.frame_set_layout};
        const uint32_t set_count =
            desc_.frame_set_layout != VK_NULL_HANDLE ? 2 : 1;
        const VkPushConstantRange push_range{
            .stageFlags = kPushStages,
            .offset = 0,
            .size = desc_.push_constant_size,
        };
        pipeline_ = std::make_unique<Pipeline>(
            device_,
            GraphicsPipelineDesc{
                .vertex_spirv = vertex,
                .fragment_spirv = fragment,
                .set_layouts = std::span(set_layouts).first(set_count),
                .push_constants = {&push_range,
                                   desc_.push_constant_size > 0 ? 1u : 0u},
                .color_format = desc_.output_format,
                .cull_mode = VK_CULL_MODE_NONE,
                .depth_write = false,
                .blend = desc_.blend,
            });
      });
}

FullscreenPass::~FullscreenPass() {
  vkDestroyDescriptorPool(device_, pool_, nullptr);
  vkDestroyDescriptorSetLayout(device_, input_layout_, nullptr);
}

void FullscreenPass::SetInputs(uint32_t set_index,
                               std::span<const VkImageView> views,
                               VkSampler sampler, VkImageLayout layout) {
  if (set_index >= sets_.size() || views.size() != desc_.input_count) {
    core::log::Fatal("Bad inputs for full-screen pass '{}'",
                     desc_.fragment_path);
  }
  std::vector<VkDescriptorImageInfo> infos(views.size());
  std::vector<VkWriteDescriptorSet> writes(views.size());
  for (size_t i = 0; i < views.size(); ++i) {
    infos[i] = {
        .sampler = sampler,
        .imageView = views[i],
        .imageLayout = layout,
    };
    writes[i] = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = sets_[set_index],
        .dstBinding = static_cast<uint32_t>(i),
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = &infos[i],
    };
  }
  vkUpdateDescriptorSets(device_, static_cast<uint32_t>(writes.size()),
                         writes.data(), 0, nullptr);
}

void FullscreenPass::Record(VkCommandBuffer cmd, const FullscreenTarget& target,
                            uint32_t set_index, std::span<const std::byte> push,
                            VkDescriptorSet frame_set) const {
  const VkRenderingAttachmentInfo color_attachment{
      .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
      .imageView = target.view,
      .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      .loadOp = target.load_op,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
  };
  const VkRect2D area{{0, 0}, target.extent};
  const VkRenderingInfo rendering_info{
      .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
      .renderArea = area,
      .layerCount = 1,
      .colorAttachmentCount = 1,
      .pColorAttachments = &color_attachment,
  };
  vkCmdBeginRendering(cmd, &rendering_info);

  // Not flipped: y = 0 is the top row, matching inUv.
  const VkViewport viewport{
      .x = 0.0f,
      .y = 0.0f,
      .width = static_cast<float>(target.extent.width),
      .height = static_cast<float>(target.extent.height),
      .minDepth = 0.0f,
      .maxDepth = 1.0f,
  };
  vkCmdSetViewport(cmd, 0, 1, &viewport);
  vkCmdSetScissor(cmd, 0, 1, &area);

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_->handle());
  const std::array<VkDescriptorSet, 2> sets{sets_[set_index], frame_set};
  const uint32_t set_count = desc_.frame_set_layout != VK_NULL_HANDLE ? 2 : 1;
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          pipeline_->layout(), 0, set_count, sets.data(), 0,
                          nullptr);
  if (!push.empty()) {
    vkCmdPushConstants(cmd, pipeline_->layout(), kPushStages, 0,
                       static_cast<uint32_t>(push.size_bytes()), push.data());
  }
  vkCmdDraw(cmd, kFullscreenVertexCount, 1, 0, 0);
  vkCmdEndRendering(cmd);
}

}  // namespace blue::render
