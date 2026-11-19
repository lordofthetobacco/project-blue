#include "render/passes/overlay_pass.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>

#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include "core/path.h"
#include "render/debug_lines.h"
#include "render/encoders.h"
#include "render/passes/scene_pass.h"
#include "shaders/bindings.h"

namespace blue::render {

namespace {

// Initial line buffer size; grows to the next power of two as needed.
constexpr size_t kInitialLineBufferSize = 64 * 1024;

// Must match OutlineConstants in shaders/selection_outline.frag.metal.
struct OutlineConstants {
  glm::vec4 color;  // display space, alpha = opacity
};
static_assert(sizeof(OutlineConstants) == 16);

// Returns a new vertex descriptor that matches LineVertex
// (shaders/line.vert.metal), read from the buffer at kBufferVertices.
MTLVertexDescriptor* LineVertexDescriptor() {
  MTLVertexDescriptor* descriptor = [MTLVertexDescriptor vertexDescriptor];
  MTLVertexAttributeDescriptor* position =
      descriptor.attributes[kAttributePosition];
  position.format = MTLVertexFormatFloat3;
  position.offset = offsetof(LineVertex, position);
  position.bufferIndex = kBufferVertices;
  MTLVertexAttributeDescriptor* color =
      descriptor.attributes[kAttributeLineColor];
  color.format = MTLVertexFormatUChar4Normalized;
  color.offset = offsetof(LineVertex, color);
  color.bufferIndex = kBufferVertices;
  descriptor.layouts[kBufferVertices].stride = sizeof(LineVertex);
  return descriptor;
}

// Begins rendering into `color` (loaded, or cleared to 0) with the scene
// depth as a depth attachment that nothing writes, on the scene viewport so
// that lines and the mask land on the same pixels as the scene depth.
id<MTLRenderCommandEncoder> BeginOverlayRendering(const FrameContext& ctx,
                                                  id<MTLTexture> color,
                                                  MTLLoadAction color_load,
                                                  const char* label) {
  MTLRenderPassDescriptor* descriptor =
      [MTLRenderPassDescriptor renderPassDescriptor];
  MTLRenderPassColorAttachmentDescriptor* color_attachment =
      descriptor.colorAttachments[0];
  color_attachment.texture = color;
  color_attachment.loadAction = color_load;
  color_attachment.storeAction = MTLStoreActionStore;
  color_attachment.clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 0.0);
  MTLRenderPassDepthAttachmentDescriptor* depth = descriptor.depthAttachment;
  depth.texture = ctx.targets.depth().texture();
  depth.loadAction = MTLLoadActionLoad;
  // Unchanged by the overlay; kept for its next encoder.
  depth.storeAction = MTLStoreActionStore;

  const id<MTLRenderCommandEncoder> encoder =
      BeginRenderPass(ctx, descriptor, label);
  SetSceneViewport(encoder, ctx.targets.extent());
  ctx.globals.Bind(encoder, ctx.frame_index);
  return encoder;
}

}  // namespace

OverlayPass::OverlayPass(const MetalContext& context, ShaderPrograms& programs,
                         const std::string& shader_dir,
                         const std::vector<Mesh>& meshes)
    : context_(context),
      meshes_(meshes),
      outline_(context, programs, shader_dir,
               FullscreenPassDesc{
                   .fragment_path = core::JoinPath(
                       shader_dir, "selection_outline.frag.metal"),
                   .input_count = 1,
                   .pass_constant_size = sizeof(OutlineConstants),
                   .output_format = RenderTargets::kLdrFormat,
                   .blend = BlendMode::kAlpha,
                   .label = "selection outline",
               }) {
  id<MTLDevice> device = context.device();
  programs.Add(
      core::JoinPath(shader_dir, "line.vert.metal"),
      core::JoinPath(shader_dir, "line.frag.metal"),
      [this, device](
          const shader::ShaderLibrary& vertex,
          const shader::ShaderLibrary* fragment) -> std::optional<std::string> {
        const auto make = [&](MTLCompareFunction compare, const char* label) {
          return Pipeline::Create(
              device, GraphicsPipelineDesc{
                          .vertex = &vertex,
                          .fragment = fragment,
                          .vertex_descriptor = LineVertexDescriptor(),
                          .color_format = RenderTargets::kLdrFormat,
                          .depth_format = RenderTargets::kDepthFormat,
                          .cull_mode = MTLCullModeNone,
                          .depth_compare = compare,
                          .depth_write = false,
                          .blend = BlendMode::kAlpha,
                          .topology = MTLPrimitiveTopologyClassLine,
                          .label = label,
                      });
        };
        // Reversed Z: nearer is greater.
        auto tested =
            make(MTLCompareFunctionGreaterEqual, "debug lines (depth tested)");
        if (!tested.ok()) {
          return tested.error();
        }
        auto top = make(MTLCompareFunctionAlways, "debug lines (on top)");
        if (!top.ok()) {
          return top.error();
        }
        tested_lines_ = std::move(tested).value();
        top_lines_ = std::move(top).value();
        return std::nullopt;
      });
  programs.Add(
      core::JoinPath(shader_dir, "depth.vert.metal"),
      core::JoinPath(shader_dir, "selection_mask.frag.metal"),
      [this, device](
          const shader::ShaderLibrary& vertex,
          const shader::ShaderLibrary* fragment) -> std::optional<std::string> {
        const auto make = [&](bool visible) {
          // Element i is function constant i; only kConstantSelectionVisible
          // is declared by selection_mask.frag.metal.
          std::array<bool, kConstantSelectionVisible + 1> constants{};
          constants[kConstantSelectionVisible] = visible;
          // depth.vert.metal's invariant position matches the depth pre-pass
          // exactly, so the visible test can compare GreaterEqual.
          return Pipeline::Create(
              device,
              GraphicsPipelineDesc{
                  .vertex = &vertex,
                  .fragment = fragment,
                  .fragment_constants = constants,
                  .vertex_descriptor = MeshVertexDescriptor(),
                  .color_format = RenderTargets::kSelectionMaskFormat,
                  .depth_format = RenderTargets::kDepthFormat,
                  .cull_mode = MTLCullModeNone,
                  .depth_compare = visible ? MTLCompareFunctionGreaterEqual
                                           : MTLCompareFunctionAlways,
                  .depth_write = false,
                  .label = visible ? "selection mask (visible)"
                                   : "selection mask (hidden)",
              });
        };
        auto hidden = make(/*visible=*/false);
        if (!hidden.ok()) {
          return hidden.error();
        }
        auto visible = make(/*visible=*/true);
        if (!visible.ok()) {
          return visible.error();
        }
        mask_hidden_ = std::move(hidden).value();
        mask_visible_ = std::move(visible).value();
        return std::nullopt;
      });
}

void OverlayPass::BindTargets(const RenderTargets& targets,
                              const Samplers& samplers) {
  const std::array<id<MTLTexture>, 1> mask{targets.selection_mask().texture()};
  outline_.SetInputs(0, mask, samplers.nearest_clamp());
}

bool OverlayPass::HasWork(const SceneView& view) {
  return !view.lines.empty() || !view.overlay_lines.empty() ||
         !view.selected_draws.empty();
}

void OverlayPass::Record(const FrameContext& ctx) {
  if (!HasWork(ctx.view)) {
    return;
  }
  if (!ctx.view.selected_draws.empty()) {
    RecordSelectionMask(ctx);
  }
  if (!ctx.view.lines.empty() || !ctx.view.overlay_lines.empty()) {
    UploadLines(ctx);
    RecordLines(ctx);
  }
  if (!ctx.view.selected_draws.empty()) {
    const OutlineConstants constants{.color = ctx.view.selection_color};
    outline_.Record(ctx,
                    {.texture = ctx.targets.ldr().texture(),
                     .extent = ctx.targets.extent(),
                     .load_action = MTLLoadActionLoad},
                    0, ConstantBytes(constants));
    ++ctx.stats.draw_calls;
  }
}

void OverlayPass::UploadLines(const FrameContext& ctx) {
  const std::span<const LineVertex> tested = ctx.view.lines;
  const std::span<const LineVertex> on_top = ctx.view.overlay_lines;
  const size_t needed = tested.size_bytes() + on_top.size_bytes();
  Buffer& buffer = line_buffers_[ctx.frame_index];
  if (buffer.size() < needed) {
    size_t size = kInitialLineBufferSize;
    while (size < needed) {
      size *= 2;
    }
    // This frame slot's previous command buffer has completed (it was
    // waited on before recording), so the buffer can be replaced right
    // away.
    buffer = Buffer(context_, size, Buffer::Access::kCpuSequentialWrite,
                    "debug lines");
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
  const id<MTLRenderCommandEncoder> encoder = BeginOverlayRendering(
      ctx, ctx.targets.ldr().texture(), MTLLoadActionLoad, "debug lines");
  [encoder setVertexBuffer:line_buffers_[ctx.frame_index].handle()
                    offset:0
                   atIndex:kBufferVertices];
  const auto draw = [&](const Pipeline& pipeline, size_t first, size_t count) {
    if (count == 0) {
      return;
    }
    pipeline.Bind(encoder);
    [encoder drawPrimitives:MTLPrimitiveTypeLine
                vertexStart:first
                vertexCount:count];
    ++ctx.stats.draw_calls;
  };
  const size_t tested_count = ctx.view.lines.size();
  draw(*tested_lines_, 0, tested_count);
  draw(*top_lines_, tested_count, ctx.view.overlay_lines.size());
  [encoder endEncoding];
}

void OverlayPass::RecordSelectionMask(const FrameContext& ctx) const {
  const id<MTLRenderCommandEncoder> encoder =
      BeginOverlayRendering(ctx, ctx.targets.selection_mask().texture(),
                            MTLLoadActionClear, "selection mask");
  // Hidden parts first (0.5), then the visible ones overwrite them (1).
  for (const Pipeline* pipeline : {mask_hidden_.get(), mask_visible_.get()}) {
    pipeline->Bind(encoder);
    for (const uint32_t index : ctx.view.selected_draws) {
      if (index >= ctx.view.draws.size()) {
        continue;
      }
      const DrawItem& item = ctx.view.draws[index];
      const auto mesh_index = static_cast<size_t>(item.mesh);
      if (mesh_index >= meshes_.size() || !meshes_[mesh_index].resident()) {
        continue;
      }
      // DrawConstants of depth.vert.metal: the model matrix.
      const glm::mat4 model = item.transform;
      [encoder setVertexBytes:&model length:sizeof(model) atIndex:kBufferDraw];
      meshes_[mesh_index].Draw(encoder);
      ++ctx.stats.draw_calls;
    }
  }
  [encoder endEncoding];
}

}  // namespace blue::render
