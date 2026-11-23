#include "render/vulkan/passes/depth_prepass.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include "render/binding_layout.h"
#include "render/shader_manifest.h"
#include "core/path.h"
#include "render/vulkan/barriers.h"
#include "render/vulkan/draw_record.h"
#include "render/vulkan/render_targets.h"

namespace blue::render::vulkan {

namespace {

// Reversed-Z: infinity is 0.
constexpr float kDepthClearValue = 0.0f;

constexpr auto kVertexBindings = VertexBindings();
constexpr auto kVertexAttributes = PositionUvVertexAttributes();

// What reads the depth target after the pre-pass: SSAO samples it, the
// scene pass depth-tests against it.
constexpr BarrierScope kDepthReaders{
    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
        VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT |
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT};

}  // namespace

DepthPrepass::DepthPrepass(const VulkanContext& context,
                           ShaderPrograms& programs,
                           VkDescriptorSetLayout frame_layout,
                           const MaterialSystem& materials,
                           const std::vector<Mesh>& meshes)
    : device_(context.device()),
      frame_layout_(frame_layout),
      materials_(materials),
      meshes_(meshes) {
  programs.Add(ShaderProgram::kDepth,
               [this](std::span<const uint32_t> vertex,
                      std::span<const uint32_t> fragment) {
                 CreatePipelines(/*masked=*/false, vertex, fragment);
               });
  programs.Add(ShaderProgram::kDepthMasked,
               [this](std::span<const uint32_t> vertex,
                      std::span<const uint32_t> fragment) {
                 CreatePipelines(/*masked=*/true, vertex, fragment);
               });
}

void DepthPrepass::CreatePipelines(bool masked,
                                   std::span<const uint32_t> vertex,
                                   std::span<const uint32_t> fragment) {
  const std::array<VkDescriptorSetLayout, 2> set_layouts{
      frame_layout_, materials_.set_layout()};
  for (size_t i = 0; i < kMaterialVariantCount; ++i) {
    const auto variant = static_cast<MaterialVariant>(i);
    if (IsMasked(variant) != masked) {
      continue;
    }
    pipelines_[i] = std::make_unique<Pipeline>(
        device_, GraphicsPipelineDesc{
                     .vertex_spirv = vertex,
                     .fragment_spirv = fragment,
                     .vertex_bindings = kVertexBindings,
                     .vertex_attributes = kVertexAttributes,
                     .set_layouts = set_layouts,
                     .push_constants = {&kDrawConstantsRange, 1},
                     .depth_format = RenderTargets::kDepthFormat,
                     .cull_mode = IsDoubleSided(variant)
                                      ? VkCullModeFlags{VK_CULL_MODE_NONE}
                                      : VkCullModeFlags{VK_CULL_MODE_BACK_BIT},
                     .depth_compare_op = VK_COMPARE_OP_GREATER,
                 });
  }
}

void DepthPrepass::Record(const FrameContext& ctx) const {
  const VkCommandBuffer cmd = ctx.cmd;
  const Image& depth = ctx.scene_depth;
  // The previous frame sampled (SSAO) and depth-tested (scene) it.
  ImageBarrier(cmd, depth.handle(), VK_IMAGE_ASPECT_DEPTH_BIT, kDepthReaders,
               kDepthAttachment, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);

  const VkExtent2D extent = ctx.scene_extent;
  const VkRenderingAttachmentInfo depth_attachment{
      .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
      .imageView = depth.view(),
      .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .clearValue = {.depthStencil = {kDepthClearValue, 0}},
  };
  const VkRenderingInfo rendering_info{
      .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
      .renderArea = {{0, 0}, extent},
      .layerCount = 1,
      .pDepthAttachment = &depth_attachment,
  };
  vkCmdBeginRendering(cmd, &rendering_info);
  // Same Y-up viewport as the scene pass (see BeginScenePass).
  const VkViewport viewport{
      .x = 0.0f,
      .y = static_cast<float>(extent.height),
      .width = static_cast<float>(extent.width),
      .height = -static_cast<float>(extent.height),
      .minDepth = 0.0f,
      .maxDepth = 1.0f,
  };
  const VkRect2D scissor{{0, 0}, extent};
  vkCmdSetViewport(cmd, 0, 1, &viewport);
  vkCmdSetScissor(cmd, 0, 1, &scissor);

  DrawListPipelines pipelines;
  for (size_t i = 0; i < kMaterialVariantCount; ++i) {
    pipelines.pipelines[i] = pipelines_[i].get();
    pipelines.bind_material[i] = IsMasked(static_cast<MaterialVariant>(i));
  }
  RecordDrawList(cmd, ctx.draw_list, pipelines, ctx.frame_set, ctx.view,
                 materials_, meshes_, /*stats=*/nullptr);
  vkCmdEndRendering(cmd);

  ImageBarrier(
      cmd, depth.handle(), VK_IMAGE_ASPECT_DEPTH_BIT,
      {kDepthAttachment.stage, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT},
      kDepthReaders, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
      VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL);
}

}  // namespace blue::render::vulkan
