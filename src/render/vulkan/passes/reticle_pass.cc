#include "render/vulkan/passes/reticle_pass.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include "render/shader_manifest.h"
#include "core/path.h"
#include "render/vulkan/render_targets.h"

namespace blue::render::vulkan {

namespace {

// Must match ReticleConstants in shaders/reticle.slang.
struct ReticleConstants {
  glm::mat4 lens;
  glm::vec4 axis;       // xyz: axis, w: dot angular radius
  glm::vec4 dot_color;  // rgb: colour, w: minimum dot diameter in pixels
  glm::vec4 tint;
};
static_assert(offsetof(ReticleConstants, axis) == 64);
static_assert(offsetof(ReticleConstants, dot_color) == 80);
static_assert(offsetof(ReticleConstants, tint) == 96);
static_assert(sizeof(ReticleConstants) <= 128,
              "Push constants must fit the guaranteed minimum of 128 bytes");

constexpr VkPushConstantRange kConstantsRange{
    .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
    .offset = 0,
    .size = sizeof(ReticleConstants),
};

// The sight quad is generated in the vertex shader from two triangles.
constexpr uint32_t kVertexCount = 6;

}  // namespace

ReticlePass::ReticlePass(const VulkanContext& context, ShaderPrograms& programs,
                         VkDescriptorSetLayout frame_layout)
    : device_(context.device()), frame_layout_(frame_layout) {
  programs.Add(ShaderProgram::kReticle,
               [this](std::span<const uint32_t> vertex,
                      std::span<const uint32_t> fragment) {
                 // Additive, depth tested against the scene (equal passes too,
                 // for a window lying on the model's own glass) without writing
                 // depth, and visible from both sides.
                 pipeline_ = std::make_unique<Pipeline>(
                     device_,
                     GraphicsPipelineDesc{
                         .vertex_spirv = vertex,
                         .fragment_spirv = fragment,
                         .set_layouts = {&frame_layout_, 1},
                         .push_constants = {&kConstantsRange, 1},
                         .color_format = RenderTargets::kHdrFormat,
                         .depth_format = RenderTargets::kDepthFormat,
                         .cull_mode = VK_CULL_MODE_NONE,
                         .depth_compare_op = VK_COMPARE_OP_GREATER_OR_EQUAL,
                         .depth_write = false,
                         .blend = BlendMode::kAdditive,
                     });
               });
}

void ReticlePass::Draw(const FrameContext& ctx) const {
  if (!ctx.view.sight) {
    return;
  }
  const ReticleSight& sight = *ctx.view.sight;
  const ReticleConstants constants{
      .lens = sight.lens,
      .axis = glm::vec4(sight.axis, sight.dot_angular_radius),
      .dot_color = glm::vec4(sight.dot_color, sight.min_dot_pixels),
      .tint = glm::vec4(sight.tint, 0.0f),
  };
  vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    pipeline_->handle());
  vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          pipeline_->layout(), 0, 1, &ctx.frame_set, 0,
                          nullptr);
  vkCmdPushConstants(ctx.cmd, pipeline_->layout(), kConstantsRange.stageFlags,
                     0, sizeof(constants), &constants);
  vkCmdDraw(ctx.cmd, kVertexCount, 1, 0, 0);
  ++ctx.stats.draw_calls;
}

}  // namespace blue::render::vulkan
