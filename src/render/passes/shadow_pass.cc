#include "render/passes/shadow_pass.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include <glm/mat4x4.hpp>

#include "core/path.h"
#include "lighting/cascades.h"
#include "render/barriers.h"
#include "render/vk_check.h"

namespace blue::render {

namespace {

// Depth bias (see vkCmdSetDepthBias); tune visually.
constexpr float kDepthBiasConstant = 1.25f;
constexpr float kDepthBiasClamp = 0.0f;
constexpr float kDepthBiasSlope = 1.75f;
// The cascades use standard depth: the far plane is 1.
constexpr float kDepthClearValue = 1.0f;

// Must match ShadowConstants in shaders/shadow.vert.
struct ShadowConstants {
  glm::mat4 light_model_view_proj;
};
static_assert(sizeof(ShadowConstants) == 64);

constexpr auto kVertexBindings = VertexBindings();
constexpr auto kVertexAttributes = PositionOnlyVertexAttributes();
constexpr VkPushConstantRange kConstantsRange{
    .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
    .offset = 0,
    .size = sizeof(ShadowConstants),
};

ImageDesc MapDesc() {
  return {
      .extent = {lighting::kShadowMapSize, lighting::kShadowMapSize},
      .format = ShadowPass::kFormat,
      .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
               VK_IMAGE_USAGE_SAMPLED_BIT,
      .aspect = VK_IMAGE_ASPECT_DEPTH_BIT,
      .array_layers = lighting::kCascadeCount,
      .dedicated = true,
  };
}

// Returns a sampler that compares against the stored depth: linear filtering
// makes the hardware average the 2x2 comparison results (PCF), and positions
// outside the map read as depth 1, i.e. lit.
VkSampler CreateSampler(VkDevice device) {
  const VkSamplerCreateInfo create_info{
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = VK_FILTER_LINEAR,
      .minFilter = VK_FILTER_LINEAR,
      .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
      .compareEnable = VK_TRUE,
      .compareOp = VK_COMPARE_OP_LESS_OR_EQUAL,
      .minLod = 0.0f,
      .maxLod = 0.0f,
      .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE,
  };
  VkSampler sampler = VK_NULL_HANDLE;
  CheckVk(vkCreateSampler(device, &create_info, nullptr, &sampler));
  return sampler;
}

// Depth-only pipeline: no fragment stage, no color.
GraphicsPipelineDesc PipelineDesc(std::span<const uint32_t> vertex_spirv) {
  return {
      .vertex_spirv = vertex_spirv,
      .vertex_bindings = kVertexBindings,
      .vertex_attributes = kVertexAttributes,
      .push_constants = {&kConstantsRange, 1},
      .depth_format = ShadowPass::kFormat,
      // Open meshes (teapot rim, ground) must cast from both sides.
      .cull_mode = VK_CULL_MODE_NONE,
      // Cascades use standard depth (near 0, far 1), unlike the main camera.
      .depth_compare_op = VK_COMPARE_OP_LESS,
      .depth_clamp = true,
      .depth_bias = true,
  };
}

}  // namespace

ShadowPass::ShadowPass(const VulkanContext& context, ShaderPrograms& programs,
                       const std::string& shader_dir,
                       const std::vector<Mesh>& meshes)
    : device_(context.device()),
      meshes_(meshes),
      map_(context, MapDesc()),
      sampler_(CreateSampler(device_)) {
  programs.Add(core::JoinPath(shader_dir, "shadow.vert"), "",
               [this](std::span<const uint32_t> vertex,
                      std::span<const uint32_t> /*fragment*/) {
                 pipeline_ =
                     std::make_unique<Pipeline>(device_, PipelineDesc(vertex));
               });
}

ShadowPass::~ShadowPass() { vkDestroySampler(device_, sampler_, nullptr); }

void ShadowPass::InitializeLayout(VkCommandBuffer cmd) {
  if (initialized_) {
    return;
  }
  ImageBarrier(cmd, map_.handle(), VK_IMAGE_ASPECT_DEPTH_BIT, kNoAccess,
               kFragmentSampledRead, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL);
  initialized_ = true;
}

void ShadowPass::Record(const FrameContext& ctx) {
  const VkCommandBuffer cmd = ctx.cmd;
  if (!ctx.shadows) {
    InitializeLayout(cmd);
    return;
  }
  constexpr uint32_t kMapSize = lighting::kShadowMapSize;

  // Moves all layers at once; their old contents are cleared on load.
  ImageBarrier(cmd, map_.handle(), VK_IMAGE_ASPECT_DEPTH_BIT,
               kFragmentSampledRead, kDepthAttachment,
               VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
  initialized_ = true;

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_->handle());
  const VkViewport viewport{
      .x = 0.0f,
      .y = 0.0f,
      .width = static_cast<float>(kMapSize),
      .height = static_cast<float>(kMapSize),
      .minDepth = 0.0f,
      .maxDepth = 1.0f,
  };
  const VkRect2D scissor{{0, 0}, {kMapSize, kMapSize}};

  for (uint32_t layer = 0; layer < ctx.cascades.size(); ++layer) {
    const VkRenderingAttachmentInfo depth_attachment{
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = map_.layer_view(layer),
        .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = {.depthStencil = {kDepthClearValue, 0}},
    };
    const VkRenderingInfo rendering_info{
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = scissor,
        .layerCount = 1,
        .pDepthAttachment = &depth_attachment,
    };
    vkCmdBeginRendering(cmd, &rendering_info);
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdSetDepthBias(cmd, kDepthBiasConstant, kDepthBiasClamp,
                      kDepthBiasSlope);

    // The cascade's box already extends towards the sun (kCasterExtension),
    // so anything outside it cannot shadow this slice.
    const lighting::Cascade& cascade = ctx.cascades[layer];
    const camera::Frustum cascade_frustum(cascade.view_projection);
    for (size_t i = 0; i < ctx.view.draws.size(); ++i) {
      if (!cascade_frustum.Intersects(ctx.world_bounds[i])) {
        ++ctx.stats.shadow_culled_draws;
        continue;
      }
      const DrawItem& item = ctx.view.draws[i];
      const auto mesh_index = static_cast<size_t>(item.mesh);
      if (mesh_index >= meshes_.size() || !meshes_[mesh_index].resident()) {
        continue;
      }
      ++ctx.stats.shadow_draw_calls;
      const ShadowConstants constants{
          .light_model_view_proj = cascade.view_projection * item.transform,
      };
      vkCmdPushConstants(cmd, pipeline_->layout(), kConstantsRange.stageFlags,
                         0, sizeof(constants), &constants);
      meshes_[mesh_index].Draw(cmd);
    }
    vkCmdEndRendering(cmd);
  }

  ImageBarrier(cmd, map_.handle(), VK_IMAGE_ASPECT_DEPTH_BIT,
               {VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                    VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT},
               kFragmentSampledRead, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
               VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL);
}

}  // namespace blue::render
