#include "render/passes/motion_blur_pass.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

#include <glm/common.hpp>
#include <glm/ext/vector_int4.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>

#include "camera/frustum.h"
#include "core/path.h"
#include "render/barriers.h"
#include "render/draw_list.h"
#include "render/material_system.h"
#include "render/motion_blur_math.h"

namespace blue::render {

namespace {

// Must match VelocityConstants in shaders/motion_velocity.vert.
struct VelocityConstants {
  glm::mat4 model;
  glm::mat4 previous_model;
};
static_assert(sizeof(VelocityConstants) <= 128,
              "Push constants must fit the guaranteed minimum of 128 bytes");

// Must match TileMaxConstants in shaders/motion_tile_max.frag.
struct TileMaxConstants {
  glm::ivec4 area;  // xy: min pixel, zw: max pixel (exclusive)
  int32_t tile_size;
  float max_length;
};

// Must match BlurConstants in shaders/motion_blur.frag.
struct BlurConstants {
  float max_length;
  int32_t tile_size;
};

constexpr VkPushConstantRange kVelocityConstantsRange{
    .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
    .offset = 0,
    .size = sizeof(VelocityConstants),
};

constexpr auto kVertexBindings = VertexBindings();
constexpr auto kVertexAttributes = PositionOnlyVertexAttributes();

// "No motion": a zero blur vector (0.5 in the encoding) and no coverage
// (shaders/motion_blur.glsl, EncodeBlur).
constexpr VkClearColorValue kNoMotion{{0.5f, 0.5f, 0.0f, 0.0f}};

// The HDR target after the scene pass: written as a color attachment, then
// made readable by fragment shaders (EndScenePass()).
constexpr BarrierScope kSceneHdrWritten{
    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT |
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT};
constexpr BarrierScope kCopyRead{VK_PIPELINE_STAGE_2_COPY_BIT,
                                 VK_ACCESS_2_TRANSFER_READ_BIT};
constexpr BarrierScope kCopyWrite{VK_PIPELINE_STAGE_2_COPY_BIT,
                                  VK_ACCESS_2_TRANSFER_WRITE_BIT};

}  // namespace

MotionBlurPass::MotionBlurPass(const VulkanContext& context,
                               ShaderPrograms& programs,
                               const std::string& shader_dir,
                               VkDescriptorSetLayout frame_layout,
                               const std::vector<Mesh>& meshes)
    : device_(context.device()),
      frame_layout_(frame_layout),
      meshes_(meshes),
      tile_max_(
          context, programs, shader_dir,
          {.fragment_path = core::JoinPath(shader_dir, "motion_tile_max.frag"),
           .push_constant_size = sizeof(TileMaxConstants),
           .output_format = RenderTargets::kMotionTileFormat}),
      blur_(context, programs, shader_dir,
            {.fragment_path = core::JoinPath(shader_dir, "motion_blur.frag"),
             .input_count = 3,
             .push_constant_size = sizeof(BlurConstants),
             .output_format = RenderTargets::kHdrFormat}) {
  programs.Add(
      core::JoinPath(shader_dir, "motion_velocity.vert"),
      core::JoinPath(shader_dir, "motion_velocity.frag"),
      [this](std::span<const uint32_t> vertex,
             std::span<const uint32_t> fragment) {
        for (size_t i = 0; i < velocity_pipelines_.size(); ++i) {
          const bool double_sided = i == 1;
          velocity_pipelines_[i] = std::make_unique<Pipeline>(
              device_,
              GraphicsPipelineDesc{
                  .vertex_spirv = vertex,
                  .fragment_spirv = fragment,
                  .vertex_bindings = kVertexBindings,
                  .vertex_attributes = kVertexAttributes,
                  .set_layouts = {&frame_layout_, 1},
                  .push_constants = {&kVelocityConstantsRange, 1},
                  .color_format = RenderTargets::kVelocityFormat,
                  .depth_format = RenderTargets::kDepthFormat,
                  .cull_mode = double_sided
                                   ? VkCullModeFlags{VK_CULL_MODE_NONE}
                                   : VkCullModeFlags{VK_CULL_MODE_BACK_BIT},
                  // Only the surface the depth pre-pass kept, as in the mesh
                  // pass (the invariant gl_Position matches depth.vert).
                  // Alpha-masked cut-outs hold the depth of what is behind
                  // them, so they fail the test without a material.
                  .depth_compare_op = VK_COMPARE_OP_EQUAL,
                  .depth_write = false,
              });
        }
      });
}

void MotionBlurPass::BindTargets(const RenderTargets& targets,
                                 const Samplers& samplers) {
  // Every input is fetched texel by texel.
  const std::array<VkImageView, 1> velocity{targets.velocity().view()};
  tile_max_.SetInputs(0, velocity, samplers.nearest_clamp());
  const std::array<VkImageView, 3> blur_inputs{targets.motion_source().view(),
                                               targets.velocity().view(),
                                               targets.motion_tiles().view()};
  blur_.SetInputs(0, blur_inputs, samplers.nearest_clamp());
}

std::optional<VkRect2D> MotionBlurPass::FindArea(const FrameContext& ctx) {
  const PostSettings& post = ctx.view.post;
  if (!post.weapon_motion_blur_enabled ||
      MotionBlurScale(post.weapon_motion_blur_strength,
                      ctx.view.frame_seconds) <= 0.0f) {
    return std::nullopt;
  }
  const VkExtent2D extent = ctx.targets.extent();
  const glm::vec2 size(static_cast<float>(extent.width),
                       static_cast<float>(extent.height));
  const glm::mat4 view_proj = ctx.view.camera.projection * ctx.view.camera.view;
  // Every covered pixel lies inside `bounds`: the blur relies on it (taps
  // outside the copied area may only be background taps).
  std::optional<ScreenBounds> bounds;
  uint32_t added_item = UINT32_MAX;
  for (const DrawEntry& entry : ctx.draw_list) {
    if (!ctx.view.draws[entry.item].previous_transform ||
        entry.item == added_item) {
      continue;
    }
    added_item = entry.item;
    const camera::Aabb& box = ctx.world_bounds[entry.item];
    const std::optional<ScreenBounds> item_bounds =
        ProjectBoxToScreen(box.min, box.max, view_proj, size);
    if (!item_bounds) {
      continue;  // Nothing of it can be rasterized.
    }
    bounds = bounds ? ScreenBounds{glm::min(bounds->min, item_bounds->min),
                                   glm::max(bounds->max, item_bounds->max)}
                    : *item_bounds;
  }
  if (!bounds) {
    return std::nullopt;
  }
  if (!std::isfinite(bounds->min.x) || !std::isfinite(bounds->min.y) ||
      !std::isfinite(bounds->max.x) || !std::isfinite(bounds->max.y)) {
    return VkRect2D{{0, 0}, extent};  // Degenerate matrices.
  }
  // Blur reaches at most the largest radius past the items' pixels; one more
  // pixel covers rounding.
  const float margin =
      static_cast<float>(ctx.targets.motion_tile_size()) + 1.0f;
  const glm::vec2 low =
      glm::clamp(glm::floor(bounds->min - margin), glm::vec2(0.0f), size);
  const glm::vec2 high =
      glm::clamp(glm::ceil(bounds->max + margin), glm::vec2(0.0f), size);
  if (!(high.x > low.x) || !(high.y > low.y)) {
    return std::nullopt;  // Entirely off screen.
  }
  return VkRect2D{{static_cast<int32_t>(low.x), static_cast<int32_t>(low.y)},
                  {static_cast<uint32_t>(high.x - low.x),
                   static_cast<uint32_t>(high.y - low.y)}};
}

void MotionBlurPass::Record(const FrameContext& ctx,
                            const VkRect2D& area) const {
  RecordVelocity(ctx);
  RecordTileMax(ctx, area);
  RecordSourceCopy(ctx, area);

  const RenderTargets& targets = ctx.targets;
  const BlurConstants constants{
      .max_length = 2.0f * static_cast<float>(targets.motion_tile_size()),
      .tile_size = targets.motion_tile_size(),
  };
  blur_.Record(ctx.cmd,
               {.view = targets.hdr().view(),
                .extent = targets.extent(),
                .load_op = VK_ATTACHMENT_LOAD_OP_LOAD,
                .area = area},
               0, PushBytes(constants));
  ImageBarrier(ctx.cmd, targets.hdr().handle(), VK_IMAGE_ASPECT_COLOR_BIT,
               kColorAttachmentReadWrite, kFragmentSampledRead,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void MotionBlurPass::RecordVelocity(const FrameContext& ctx) const {
  const VkCommandBuffer cmd = ctx.cmd;
  const Image& velocity = ctx.targets.velocity();
  // The previous frame's tile max and blur sampled it.
  ImageBarrier(cmd, velocity.handle(), VK_IMAGE_ASPECT_COLOR_BIT,
               kFragmentSampledRead, kColorAttachmentWrite,
               VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  // The depth target needs no barrier: the pre-pass made it visible to the
  // depth tests and left it read-only, as for the scene pass.

  const VkExtent2D extent = ctx.targets.extent();
  const VkRenderingAttachmentInfo color_attachment{
      .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
      .imageView = velocity.view(),
      .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .clearValue = {.color = kNoMotion},
  };
  const VkRenderingAttachmentInfo depth_attachment{
      .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
      .imageView = ctx.targets.depth().view(),
      .imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
      .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
      .storeOp = VK_ATTACHMENT_STORE_OP_NONE,
  };
  const VkRenderingInfo rendering_info{
      .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
      .renderArea = {{0, 0}, extent},
      .layerCount = 1,
      .colorAttachmentCount = 1,
      .pColorAttachments = &color_attachment,
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

  const Pipeline* bound_pipeline = nullptr;
  const Mesh* bound_mesh = nullptr;
  uint32_t pushed_item = UINT32_MAX;
  for (const DrawEntry& entry : ctx.draw_list) {
    const DrawItem& item = ctx.view.draws[entry.item];
    if (!item.previous_transform) {
      continue;
    }
    const Pipeline* pipeline =
        velocity_pipelines_[IsDoubleSided(entry.variant) ? 1 : 0].get();
    if (pipeline != bound_pipeline) {
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        pipeline->handle());
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              pipeline->layout(), 0, 1, &ctx.frame_set, 0,
                              nullptr);
      bound_pipeline = pipeline;
      pushed_item = UINT32_MAX;
    }
    // DrawList::Build() only keeps entries of resident meshes.
    const Mesh& mesh = meshes_[static_cast<size_t>(item.mesh)];
    if (&mesh != bound_mesh) {
      mesh.Bind(cmd);
      bound_mesh = &mesh;
    }
    if (entry.item != pushed_item) {
      const VelocityConstants constants{
          .model = item.transform,
          .previous_model = *item.previous_transform,
      };
      vkCmdPushConstants(cmd, pipeline->layout(),
                         kVelocityConstantsRange.stageFlags, 0,
                         sizeof(constants), &constants);
      pushed_item = entry.item;
    }
    Mesh::DrawRange(cmd, entry.first_index, entry.index_count);
  }
  vkCmdEndRendering(cmd);

  ImageBarrier(cmd, velocity.handle(), VK_IMAGE_ASPECT_COLOR_BIT,
               kColorAttachmentWrite, kFragmentSampledRead,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void MotionBlurPass::RecordTileMax(const FrameContext& ctx,
                                   const VkRect2D& area) const {
  const Image& tiles = ctx.targets.motion_tiles();
  // The previous frame's blur sampled it.
  ImageBarrier(ctx.cmd, tiles.handle(), VK_IMAGE_ASPECT_COLOR_BIT,
               kFragmentSampledRead, kColorAttachmentWrite,
               VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  // Every tile is written (those outside `area` with zero, cheaply), so the
  // blur's 3 x 3 neighbourhood never reads a stale one.
  const TileMaxConstants constants{
      .area =
          glm::ivec4(area.offset.x, area.offset.y,
                     area.offset.x + static_cast<int32_t>(area.extent.width),
                     area.offset.y + static_cast<int32_t>(area.extent.height)),
      .tile_size = ctx.targets.motion_tile_size(),
      .max_length = 2.0f * static_cast<float>(ctx.targets.motion_tile_size()),
  };
  tile_max_.Record(ctx.cmd, {.view = tiles.view(), .extent = tiles.extent()}, 0,
                   PushBytes(constants));
  ImageBarrier(ctx.cmd, tiles.handle(), VK_IMAGE_ASPECT_COLOR_BIT,
               kColorAttachmentWrite, kFragmentSampledRead,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void MotionBlurPass::RecordSourceCopy(const FrameContext& ctx,
                                      const VkRect2D& area) const {
  const VkCommandBuffer cmd = ctx.cmd;
  const VkImage hdr = ctx.targets.hdr().handle();
  const VkImage source = ctx.targets.motion_source().handle();
  ImageBarrier(cmd, hdr, VK_IMAGE_ASPECT_COLOR_BIT, kSceneHdrWritten, kCopyRead,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  // The previous frame's blur sampled it; what is outside `area` is never
  // read.
  ImageBarrier(cmd, source, VK_IMAGE_ASPECT_COLOR_BIT, kFragmentSampledRead,
               kCopyWrite, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  const VkImageCopy region{
      .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
      .srcOffset = {area.offset.x, area.offset.y, 0},
      .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
      .dstOffset = {area.offset.x, area.offset.y, 0},
      .extent = {area.extent.width, area.extent.height, 1},
  };
  vkCmdCopyImage(cmd, hdr, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, source,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  ImageBarrier(cmd, source, VK_IMAGE_ASPECT_COLOR_BIT, kCopyWrite,
               kFragmentSampledRead, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  // The blur loads the HDR image and rewrites `area` of it.
  ImageBarrier(cmd, hdr, VK_IMAGE_ASPECT_COLOR_BIT,
               {VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_NONE},
               kColorAttachmentReadWrite, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
}

}  // namespace blue::render
