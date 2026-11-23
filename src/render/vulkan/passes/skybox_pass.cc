#include "render/vulkan/passes/skybox_pass.h"

#include <array>
#include <cstdint>
#include <memory>
#include <span>

#include "core/path.h"
#include "render/vulkan/render_targets.h"
#include "render/vulkan/vk_check.h"

namespace blue::render::vulkan {

namespace {

// The vertex shader emits one triangle that covers the whole viewport.
constexpr uint32_t kVertexCount = 3;

}  // namespace

SkyboxPass::SkyboxPass(const VulkanContext& context, ShaderPrograms& programs,
                       const std::string& shader_dir,
                       VkDescriptorSetLayout frame_layout,
                       const Environment& environment)
    : device_(context.device()), frame_layout_(frame_layout) {
  const VkDescriptorSetLayoutBinding binding{
      .binding = 0,
      .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      .descriptorCount = 1,
      .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
  };
  const VkDescriptorSetLayoutCreateInfo layout_info{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 1,
      .pBindings = &binding,
  };
  CheckVk(vkCreateDescriptorSetLayout(device_, &layout_info, nullptr,
                                      &sky_layout_));

  const VkDescriptorPoolSize pool_size{
      .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      .descriptorCount = 1,
  };
  const VkDescriptorPoolCreateInfo pool_info{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = 1,
      .poolSizeCount = 1,
      .pPoolSizes = &pool_size,
  };
  CheckVk(vkCreateDescriptorPool(device_, &pool_info, nullptr, &pool_));
  const VkDescriptorSetAllocateInfo set_info{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = pool_,
      .descriptorSetCount = 1,
      .pSetLayouts = &sky_layout_,
  };
  CheckVk(vkAllocateDescriptorSets(device_, &set_info, &sky_set_));
  const VkDescriptorImageInfo image_info{
      .sampler = environment.sky_sampler(),
      .imageView = environment.sky_view(),
      .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
  };
  const VkWriteDescriptorSet write{
      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .dstSet = sky_set_,
      .dstBinding = 0,
      .descriptorCount = 1,
      .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      .pImageInfo = &image_info,
  };
  vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);

  programs.Add(core::JoinPath(shader_dir, "skybox.vert"),
               core::JoinPath(shader_dir, "skybox.frag"),
               [this](std::span<const uint32_t> vertex,
                      std::span<const uint32_t> fragment) {
                 const std::array<VkDescriptorSetLayout, 2> set_layouts{
                     frame_layout_, sky_layout_};
                 // Only where nothing was drawn: the depth there is still
                 // the clear value 0, which the triangle at depth 0 equals.
                 pipeline_ = std::make_unique<Pipeline>(
                     device_,
                     GraphicsPipelineDesc{
                         .vertex_spirv = vertex,
                         .fragment_spirv = fragment,
                         .set_layouts = set_layouts,
                         .color_format = RenderTargets::kHdrFormat,
                         .depth_format = RenderTargets::kDepthFormat,
                         .cull_mode = VK_CULL_MODE_NONE,
                         .depth_compare_op = VK_COMPARE_OP_GREATER_OR_EQUAL,
                         .depth_write = false,
                     });
               });
}

SkyboxPass::~SkyboxPass() {
  vkDestroyDescriptorPool(device_, pool_, nullptr);
  vkDestroyDescriptorSetLayout(device_, sky_layout_, nullptr);
}

void SkyboxPass::Draw(const FrameContext& ctx) const {
  vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    pipeline_->handle());
  const std::array<VkDescriptorSet, 2> sets{ctx.frame_set, sky_set_};
  vkCmdBindDescriptorSets(
      ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_->layout(), 0,
      static_cast<uint32_t>(sets.size()), sets.data(), 0, nullptr);
  vkCmdDraw(ctx.cmd, kVertexCount, 1, 0, 0);
  ++ctx.stats.draw_calls;
}

}  // namespace blue::render::vulkan
