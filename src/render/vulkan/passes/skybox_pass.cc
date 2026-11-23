#include "render/vulkan/passes/skybox_pass.h"

#include <array>
#include <cstdint>
#include <memory>
#include <span>

#include "render/binding_layout.h"
#include "render/shader_manifest.h"
#include "render/vulkan/descriptor_layout.h"
#include "core/path.h"
#include "render/vulkan/render_targets.h"
#include "render/vulkan/vk_check.h"

namespace blue::render::vulkan {

namespace {

// The vertex shader emits one triangle that covers the whole viewport.
constexpr uint32_t kVertexCount = 3;

}  // namespace

SkyboxPass::SkyboxPass(const VulkanContext& context, ShaderPrograms& programs,
                       VkDescriptorSetLayout frame_layout,
                       const Environment& environment)
    : device_(context.device()), frame_layout_(frame_layout) {
  sky_layout_ = CreateSetLayout(device_, kSkyBindings);

  const std::array<VkDescriptorPoolSize, 2> pool_sizes{{
      {.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .descriptorCount = 1},
      {.type = VK_DESCRIPTOR_TYPE_SAMPLER, .descriptorCount = 1},
  }};
  const VkDescriptorPoolCreateInfo pool_info{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = 1,
      .poolSizeCount = static_cast<uint32_t>(pool_sizes.size()),
      .pPoolSizes = pool_sizes.data(),
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
      .imageView = environment.sky_view(),
      .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
  };
  const VkDescriptorImageInfo sampler_info{.sampler =
                                               environment.sky_sampler()};
  const std::array<VkWriteDescriptorSet, 2> writes{
      ImageWrite(sky_set_, BLUE_SKY_CUBE_BINDING, image_info),
      SamplerWrite(sky_set_, BLUE_SKY_SAMPLER_BINDING, sampler_info),
  };
  vkUpdateDescriptorSets(device_, static_cast<uint32_t>(writes.size()),
                         writes.data(), 0, nullptr);

  programs.Add(ShaderProgram::kSkybox,
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
