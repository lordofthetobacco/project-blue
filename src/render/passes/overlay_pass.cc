#include "render/passes/overlay_pass.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>

#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include "core/path.h"
#include "render/barriers.h"
#include "render/debug_lines.h"
#include "render/draw_list.h"
#include "render/image.h"

namespace blue::render {

namespace {

// Line vertex input: LineVertex (shaders/line.vert).
constexpr std::array<VkVertexInputBindingDescription, 1> kLineBindings{{
    {.binding = 0,
     .stride = sizeof(LineVertex),
     .inputRate = VK_VERTEX_INPUT_RATE_VERTEX},
}};
constexpr std::array<VkVertexInputAttributeDescription, 2> kLineAttributes{{
    {.location = 0,
     .binding = 0,
     .format = VK_FORMAT_R32G32B32_SFLOAT,
     .offset = offsetof(LineVertex, position)},
    {.location = 1,
     .binding = 0,
     .format = VK_FORMAT_R8G8B8A8_UNORM,
     .offset = offsetof(LineVertex, color)},
}};

// The selection mask reuses shaders/depth.vert (its invariant gl_Position
// matches the depth pre-pass exactly, so the visible test can compare
// GREATER_OR_EQUAL), which reads position and both uvs.
constexpr auto kMeshBindings = VertexBindings();
constexpr auto kMeshAttributes = PositionUvVertexAttributes();

// Initial line buffer size; grows to the next power of two as needed.
constexpr VkDeviceSize kInitialLineBufferSize = 64 * 1024;

// Must match OutlineConstants in shaders/selection_outline.frag.
struct OutlineConstants {
  glm::vec4 color;  // display space, alpha = opacity
};

// Sets the Y-up viewport of the scene passes (see BeginScenePass) so lines
// and the mask land on the same pixels as the scene depth.
void SetSceneViewport(VkCommandBuffer cmd, VkExtent2D extent) {
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
}

// Begins rendering into `color` (loaded or cleared to 0) with the scene
// depth as a read-only depth attachment.
void BeginOverlayRendering(const FrameContext& ctx, VkImageView color,
                           VkAttachmentLoadOp color_load) {
  const VkRenderingAttachmentInfo color_attachment{
      .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
      .imageView = color,
      .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      .loadOp = color_load,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .clearValue = {.color = {{0.0f, 0.0f, 0.0f, 0.0f}}},
  };
  const VkRenderingAttachmentInfo depth_attachment{
      .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
      .imageView = ctx.targets.depth().view(),
      .imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
      .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
      .storeOp = VK_ATTACHMENT_STORE_OP_NONE,
  };
  const VkExtent2D extent = ctx.targets.extent();
  const VkRenderingInfo rendering_info{
      .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
      .renderArea = {{0, 0}, extent},
      .layerCount = 1,
      .colorAttachmentCount = 1,
      .pColorAttachments = &color_attachment,
      .pDepthAttachment = &depth_attachment,
  };
  vkCmdBeginRendering(ctx.cmd, &rendering_info);
  SetSceneViewport(ctx.cmd, extent);
}

}  // namespace

OverlayPass::OverlayPass(const VulkanContext& context, ShaderPrograms& programs,
                         const std::string& shader_dir,
                         VkDescriptorSetLayout frame_layout,
                         const std::vector<Mesh>& meshes)
    : context_(context),
      frame_layout_(frame_layout),
      meshes_(meshes),
      outline_(context, programs, shader_dir,
               FullscreenPassDesc{
                   .fragment_path =
                       core::JoinPath(shader_dir, "selection_outline.frag"),
                   .input_count = 1,
                   .push_constant_size = sizeof(OutlineConstants),
                   .output_format = RenderTargets::kLdrFormat,
                   .blend = BlendMode::kAlpha,
               }) {
  programs.Add(core::JoinPath(shader_dir, "line.vert"),
               core::JoinPath(shader_dir, "line.frag"),
               [this](std::span<const uint32_t> vertex,
                      std::span<const uint32_t> fragment) {
                 const auto make = [&](VkCompareOp compare) {
                   return std::make_unique<Pipeline>(
                       context_.device(),
                       GraphicsPipelineDesc{
                           .vertex_spirv = vertex,
                           .fragment_spirv = fragment,
                           .vertex_bindings = kLineBindings,
                           .vertex_attributes = kLineAttributes,
                           .set_layouts = {&frame_layout_, 1},
                           .color_format = RenderTargets::kLdrFormat,
                           .depth_format = RenderTargets::kDepthFormat,
                           .cull_mode = VK_CULL_MODE_NONE,
                           .depth_compare_op = compare,
                           .depth_write = false,
                           .blend = BlendMode::kAlpha,
                           .topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
                       });
                 };
                 // Reversed Z: nearer is greater.
                 tested_lines_ = make(VK_COMPARE_OP_GREATER_OR_EQUAL);
                 top_lines_ = make(VK_COMPARE_OP_ALWAYS);
               });
  programs.Add(
      core::JoinPath(shader_dir, "depth.vert"),
      core::JoinPath(shader_dir, "selection_mask.frag"),
      [this](std::span<const uint32_t> vertex,
             std::span<const uint32_t> fragment) {
        const auto make = [&](bool visible) {
          // constant_id 0: visible (shaders/selection_mask.frag).
          const std::array<uint32_t, 1> constants{visible ? 1u : 0u};
          return std::make_unique<Pipeline>(
              context_.device(),
              GraphicsPipelineDesc{
                  .vertex_spirv = vertex,
                  .fragment_spirv = fragment,
                  .fragment_constants = constants,
                  .vertex_bindings = kMeshBindings,
                  .vertex_attributes = kMeshAttributes,
                  .set_layouts = {&frame_layout_, 1},
                  .push_constants = {&kDrawConstantsRange, 1},
                  .color_format = RenderTargets::kSelectionMaskFormat,
                  .depth_format = RenderTargets::kDepthFormat,
                  .cull_mode = VK_CULL_MODE_NONE,
                  .depth_compare_op = visible ? VK_COMPARE_OP_GREATER_OR_EQUAL
                                              : VK_COMPARE_OP_ALWAYS,
                  .depth_write = false,
              });
        };
        mask_hidden_ = make(/*visible=*/false);
        mask_visible_ = make(/*visible=*/true);
      });
}

void OverlayPass::BindTargets(const RenderTargets& targets,
                              const Samplers& samplers) {
  const VkImageView mask = targets.selection_mask().view();
  outline_.SetInputs(0, {&mask, 1}, samplers.nearest_clamp());
}

bool OverlayPass::HasWork(const SceneView& view) {
  return !view.lines.empty() || !view.overlay_lines.empty() ||
         !view.selected_draws.empty();
}

void OverlayPass::Record(const FrameContext& ctx) {
  if (!HasWork(ctx.view)) {
    return;
  }
  const VkCommandBuffer cmd = ctx.cmd;
  const Image& ldr = ctx.targets.ldr();
  // The depth target stays DEPTH_READ_ONLY from the pre-pass on, whose
  // barrier already made it visible to depth tests: no barrier needed.
  if (!ctx.view.selected_draws.empty()) {
    RecordSelectionMask(ctx);
  }
  // The tonemap pass wrote ldr; draw over it.
  ImageBarrier(cmd, ldr.handle(), VK_IMAGE_ASPECT_COLOR_BIT,
               {VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT |
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT},
               kColorAttachmentReadWrite,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  if (!ctx.view.lines.empty() || !ctx.view.overlay_lines.empty()) {
    UploadLines(ctx);
    BeginOverlayRendering(ctx, ldr.view(), VK_ATTACHMENT_LOAD_OP_LOAD);
    RecordLines(ctx);
    vkCmdEndRendering(cmd);
  }
  if (!ctx.view.selected_draws.empty()) {
    const OutlineConstants constants{.color = ctx.view.selection_color};
    outline_.Record(cmd,
                    {.view = ldr.view(),
                     .extent = ctx.targets.extent(),
                     .load_op = VK_ATTACHMENT_LOAD_OP_LOAD},
                    0, PushBytes(constants));
    ++ctx.stats.draw_calls;
  }
  ImageBarrier(cmd, ldr.handle(), VK_IMAGE_ASPECT_COLOR_BIT,
               kColorAttachmentReadWrite, kFragmentSampledRead,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void OverlayPass::UploadLines(const FrameContext& ctx) {
  const std::span<const LineVertex> tested = ctx.view.lines;
  const std::span<const LineVertex> on_top = ctx.view.overlay_lines;
  const VkDeviceSize needed = tested.size_bytes() + on_top.size_bytes();
  Buffer& buffer = line_buffers_[ctx.frame_index];
  if (buffer.size() < needed) {
    VkDeviceSize size = kInitialLineBufferSize;
    while (size < needed) {
      size *= 2;
    }
    // This frame's previous use of the buffer has finished (its fence was
    // waited on before recording), so it can be replaced right away.
    buffer =
        Buffer(context_.allocator(), size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
               Buffer::Access::kCpuSequentialWrite);
  }
  auto* bytes = static_cast<std::byte*>(buffer.mapped());
  if (!tested.empty()) {
    std::memcpy(bytes, tested.data(), tested.size_bytes());
  }
  if (!on_top.empty()) {
    std::memcpy(bytes + tested.size_bytes(), on_top.data(),
                on_top.size_bytes());
  }
}

void OverlayPass::RecordLines(const FrameContext& ctx) const {
  const VkCommandBuffer cmd = ctx.cmd;
  const VkBuffer buffer = line_buffers_[ctx.frame_index].handle();
  const VkDeviceSize offset = 0;
  vkCmdBindVertexBuffers(cmd, 0, 1, &buffer, &offset);
  const auto draw = [&](const Pipeline& pipeline, uint32_t first,
                        uint32_t count) {
    if (count == 0) {
      return;
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.handle());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipeline.layout(), 0, 1, &ctx.frame_set, 0,
                            nullptr);
    vkCmdDraw(cmd, count, 1, first, 0);
    ++ctx.stats.draw_calls;
  };
  const auto tested_count = static_cast<uint32_t>(ctx.view.lines.size());
  draw(*tested_lines_, 0, tested_count);
  draw(*top_lines_, tested_count,
       static_cast<uint32_t>(ctx.view.overlay_lines.size()));
}

void OverlayPass::RecordSelectionMask(const FrameContext& ctx) const {
  const VkCommandBuffer cmd = ctx.cmd;
  const Image& mask = ctx.targets.selection_mask();
  // The previous frame's outline filter sampled it; contents discarded.
  ImageBarrier(cmd, mask.handle(), VK_IMAGE_ASPECT_COLOR_BIT,
               kFragmentSampledRead, kColorAttachmentWrite,
               VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  BeginOverlayRendering(ctx, mask.view(), VK_ATTACHMENT_LOAD_OP_CLEAR);
  // Hidden parts first (0.5), then the visible ones overwrite them (1).
  for (const Pipeline* pipeline : {mask_hidden_.get(), mask_visible_.get()}) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->handle());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipeline->layout(), 0, 1, &ctx.frame_set, 0,
                            nullptr);
    for (const uint32_t index : ctx.view.selected_draws) {
      if (index >= ctx.view.draws.size()) {
        continue;
      }
      const DrawItem& item = ctx.view.draws[index];
      const auto mesh_index = static_cast<size_t>(item.mesh);
      if (mesh_index >= meshes_.size() || !meshes_[mesh_index].resident()) {
        continue;
      }
      const Mesh& mesh = meshes_[mesh_index];
      const glm::mat4 model = item.transform;
      vkCmdPushConstants(cmd, pipeline->layout(),
                         kDrawConstantsRange.stageFlags, 0, sizeof(model),
                         &model);
      mesh.Draw(cmd);
      ++ctx.stats.draw_calls;
    }
  }
  vkCmdEndRendering(cmd);
  ImageBarrier(cmd, mask.handle(), VK_IMAGE_ASPECT_COLOR_BIT,
               kColorAttachmentWrite, kFragmentSampledRead,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

}  // namespace blue::render
