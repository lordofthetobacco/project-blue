#include "render/metal/passes/overlay_pass.h"

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
#include "render/metal/encoders.h"
#include "render/frame_uniforms.h"
#include "render/metal/passes/scene_pass.h"
#include "shaders/metal/bindings.h"

namespace blue::render::metal {

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

// Fragment function constants of the overlay shaders: element i is function
// constant i.
using OverlayConstants = std::array<bool, kConstantSampledDepth + 1>;

// Begins rendering into `color` (loaded, or cleared to 0), an image of the
// output extent. Without the temporal scaler the scene depth is its depth
// attachment (which nothing writes) and the frame's globals are bound, so
// that lines and the mask land on the same pixels as the scene depth. With
// it the scene depth has another size and is jittered: there is no depth
// attachment, the globals' uniforms are replaced by unjittered ones for the
// output extent, and the scene depth is bound at kTextureInput0 for the
// shaders to test against (pipelines with kConstantSampledDepth).
id<MTL4RenderCommandEncoder> BeginOverlayRendering(const FrameContext& ctx,
                                                   id<MTLTexture> color,
                                                   MTLLoadAction color_load,
                                                   const Samplers& samplers,
                                                   const char* label) {
  MTL4RenderPassDescriptor* descriptor =
      [[MTL4RenderPassDescriptor alloc] init];
  MTLRenderPassColorAttachmentDescriptor* color_attachment =
      descriptor.colorAttachments[0];
  color_attachment.texture = color;
  color_attachment.loadAction = color_load;
  color_attachment.storeAction = MTLStoreActionStore;
  color_attachment.clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 0.0);
  if (!ctx.temporal) {
    MTLRenderPassDepthAttachmentDescriptor* depth = descriptor.depthAttachment;
    depth.texture = ctx.targets.depth().texture();
    depth.loadAction = MTLLoadActionLoad;
    // Unchanged by the overlay; kept for its next encoder.
    depth.storeAction = MTLStoreActionStore;
  }

  const id<MTL4RenderCommandEncoder> encoder =
      BeginRenderPass(ctx.rec, descriptor, label);
  const core::Extent2D extent = ctx.targets.output_extent();
  SetSceneViewport(encoder, extent);
  ctx.globals.Bind(ctx.rec, ctx.frame_index);
  if (ctx.temporal) {
    // Only the camera matrices and the screen size are read by the overlay
    // shaders.
    BindConstants(ctx.rec, kBufferFrame,
                  MakeFrameUniforms(ctx.view, {}, /*shadows=*/false, extent));
    BindTexture(ctx.rec, kTextureInput0, ctx.targets.depth().texture());
    BindSampler(ctx.rec, kSamplerInput, samplers.nearest_clamp());
  }
  return encoder;
}

}  // namespace

OverlayPass::OverlayPass(const MetalContext& context, ShaderPrograms& programs,
                         const std::string& shader_dir,
                         const Samplers& samplers,
                         const std::vector<Mesh>& meshes)
    : context_(context),
      meshes_(meshes),
      samplers_(samplers),
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
  programs.Add(
      core::JoinPath(shader_dir, "line.vert.metal"),
      core::JoinPath(shader_dir, "line.frag.metal"),
      [this, &context](
          const shader::metal::ShaderLibrary& vertex,
          const shader::metal::ShaderLibrary* fragment) -> std::optional<std::string> {
        // `depth`: with the scene depth attached; else for the encoder
        // without one (temporal scaler), where `tested` is done in the
        // shader.
        const auto make = [&](bool depth, bool tested, const char* label) {
          OverlayConstants constants{};
          constants[kConstantSampledDepth] = !depth && tested;
          return Pipeline::Create(
              context,
              GraphicsPipelineDesc{
                  .vertex = &vertex,
                  .fragment = fragment,
                  .fragment_constants = constants,
                  .vertex_descriptor = LineVertexDescriptor(),
                  .color_format = RenderTargets::kLdrFormat,
                  .depth_format = depth ? RenderTargets::kDepthFormat
                                        : MTLPixelFormatInvalid,
                  .cull_mode = MTLCullModeNone,
                  // Reversed Z: nearer is greater.
                  .depth_compare = tested ? MTLCompareFunctionGreaterEqual
                                          : MTLCompareFunctionAlways,
                  .depth_write = false,
                  .blend = BlendMode::kAlpha,
                  .topology = MTLPrimitiveTopologyClassLine,
                  .label = label,
              });
        };
        // All of them are created before any is replaced.
        std::array<std::unique_ptr<Pipeline>, 2> tested;
        std::array<std::unique_ptr<Pipeline>, 2> top;
        for (const bool depth : {false, true}) {
          auto tested_lines = make(depth, /*tested=*/true,
                                   "debug lines (depth tested)");
          if (!tested_lines.ok()) {
            return tested_lines.error();
          }
          auto top_lines = make(depth, /*tested=*/false, "debug lines (on top)");
          if (!top_lines.ok()) {
            return top_lines.error();
          }
          tested[depth ? 1 : 0] = std::move(tested_lines).value();
          top[depth ? 1 : 0] = std::move(top_lines).value();
        }
        tested_lines_ = std::move(tested);
        top_lines_ = std::move(top);
        return std::nullopt;
      });
  programs.Add(
      core::JoinPath(shader_dir, "depth.vert.metal"),
      core::JoinPath(shader_dir, "selection_mask.frag.metal"),
      [this, &context](
          const shader::metal::ShaderLibrary& vertex,
          const shader::metal::ShaderLibrary* fragment) -> std::optional<std::string> {
        const auto make = [&](bool depth, bool visible) {
          OverlayConstants constants{};
          constants[kConstantSelectionVisible] = visible;
          constants[kConstantSampledDepth] = !depth && visible;
          // depth.vert.metal's invariant position matches the depth pre-pass
          // exactly, so the visible test can compare GreaterEqual.
          return Pipeline::Create(
              context,
              GraphicsPipelineDesc{
                  .vertex = &vertex,
                  .fragment = fragment,
                  .fragment_constants = constants,
                  .vertex_descriptor = MeshVertexDescriptor(),
                  .color_format = RenderTargets::kSelectionMaskFormat,
                  .depth_format = depth ? RenderTargets::kDepthFormat
                                        : MTLPixelFormatInvalid,
                  .cull_mode = MTLCullModeNone,
                  .depth_compare = visible ? MTLCompareFunctionGreaterEqual
                                           : MTLCompareFunctionAlways,
                  .depth_write = false,
                  .label = visible ? "selection mask (visible)"
                                   : "selection mask (hidden)",
              });
        };
        std::array<std::unique_ptr<Pipeline>, 2> hidden;
        std::array<std::unique_ptr<Pipeline>, 2> visible;
        for (const bool depth : {false, true}) {
          auto mask_hidden = make(depth, /*visible=*/false);
          if (!mask_hidden.ok()) {
            return mask_hidden.error();
          }
          auto mask_visible = make(depth, /*visible=*/true);
          if (!mask_visible.ok()) {
            return mask_visible.error();
          }
          hidden[depth ? 1 : 0] = std::move(mask_hidden).value();
          visible[depth ? 1 : 0] = std::move(mask_visible).value();
        }
        mask_hidden_ = std::move(hidden);
        mask_visible_ = std::move(visible);
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
    outline_.Record(ctx.rec,
                    {.texture = ctx.targets.ldr().texture(),
                     .extent = ctx.targets.output_extent(),
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
    // This frame slot's previous work has completed (it was waited on
    // before recording), so the buffer can be replaced right away; the new
    // one is resident before this frame is committed.
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
  const id<MTL4RenderCommandEncoder> encoder =
      BeginOverlayRendering(ctx, ctx.targets.ldr().texture(),
                            MTLLoadActionLoad, samplers_, "debug lines");
  BindBuffer(ctx.rec, kBufferVertices,
             line_buffers_[ctx.frame_index].handle());
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
  // Index 1: the pipelines for an encoder with the scene depth attached.
  const size_t with_depth = ctx.temporal ? 0 : 1;
  draw(*tested_lines_[with_depth], 0, tested_count);
  draw(*top_lines_[with_depth], tested_count, ctx.view.overlay_lines.size());
  [encoder endEncoding];
}

void OverlayPass::RecordSelectionMask(const FrameContext& ctx) const {
  const id<MTL4RenderCommandEncoder> encoder =
      BeginOverlayRendering(ctx, ctx.targets.selection_mask().texture(),
                            MTLLoadActionClear, samplers_, "selection mask");
  const size_t with_depth = ctx.temporal ? 0 : 1;
  // Hidden parts first (0.5), then the visible ones overwrite them (1).
  for (const Pipeline* pipeline : {mask_hidden_[with_depth].get(),
                                   mask_visible_[with_depth].get()}) {
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
      BindConstants(ctx.rec, kBufferDraw, model);
      meshes_[mesh_index].Draw(ctx.rec, encoder);
      ++ctx.stats.draw_calls;
    }
  }
  [encoder endEncoding];
}

}  // namespace blue::render::metal
