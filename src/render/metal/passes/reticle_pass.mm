#include "render/metal/passes/reticle_pass.h"

#include <cstddef>
#include <optional>
#include <string>
#include <utility>

#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include "core/path.h"
#include "render/metal/encoders.h"
#include "render/metal/render_targets.h"
#include "shaders/metal/bindings.h"

namespace blue::render::metal {

namespace {

// Must match ReticleConstants in shaders/reticle.h.
struct ReticleConstants {
  glm::mat4 lens;
  glm::vec4 axis;       // xyz: axis, w: dot angular radius
  glm::vec4 dot_color;  // rgb: colour, w: minimum dot diameter in pixels
  glm::vec4 tint;
};
static_assert(offsetof(ReticleConstants, axis) == 64);
static_assert(offsetof(ReticleConstants, dot_color) == 80);
static_assert(offsetof(ReticleConstants, tint) == 96);
static_assert(sizeof(ReticleConstants) == 112);
static_assert(sizeof(ReticleConstants) <= kMaxPassConstantBytes);

// The sight quad is generated in the vertex shader from two triangles.
constexpr NSUInteger kVertexCount = 6;

ReticleConstants MakeConstants(const ReticleSight& sight) {
  return {
      .lens = sight.lens,
      .axis = glm::vec4(sight.axis, sight.dot_angular_radius),
      .dot_color = glm::vec4(sight.dot_color, sight.min_dot_pixels),
      .tint = glm::vec4(sight.tint, 0.0f),
  };
}

// Binds the constants of `sight` and draws its quad with `pipeline`.
void DrawSight(const FrameContext& ctx, id<MTL4RenderCommandEncoder> encoder,
               const Pipeline& pipeline, const ReticleSight& sight) {
  pipeline.Bind(encoder);
  BindConstants(ctx.rec, kBufferPass, MakeConstants(sight));
  [encoder drawPrimitives:MTLPrimitiveTypeTriangle
              vertexStart:0
              vertexCount:kVertexCount];
}

}  // namespace

ReticlePass::ReticlePass(const MetalContext& context, ShaderPrograms& programs,
                         const std::string& shader_dir) {
  programs.Add(
      core::JoinPath(shader_dir, "reticle.vert.metal"),
      core::JoinPath(shader_dir, "reticle.frag.metal"),
      [this, &context](const shader::metal::ShaderLibrary& vertex,
                       const shader::metal::ShaderLibrary* fragment)
          -> std::optional<std::string> {
        // Additive, depth tested against the scene (equal passes too, for a
        // window lying on the model's own glass) without writing depth, and
        // visible from both sides.
        const GraphicsPipelineDesc desc{
            .vertex = &vertex,
            .fragment = fragment,
            .color_format = RenderTargets::kHdrFormat,
            .depth_format = RenderTargets::kDepthFormat,
            .cull_mode = MTLCullModeNone,
            .depth_compare = MTLCompareFunctionGreaterEqual,
            .depth_write = false,
            .blend = BlendMode::kAdditive,
            .label = "reticle",
        };
        auto pipeline = Pipeline::Create(context, desc);
        if (!pipeline.ok()) {
          return pipeline.error();
        }
        pipeline_ = std::move(pipeline).value();
        return std::nullopt;
      });
  programs.Add(
      core::JoinPath(shader_dir, "reticle.vert.metal"),
      core::JoinPath(shader_dir, "reticle_reactive.frag.metal"),
      [this, &context](const shader::metal::ShaderLibrary& vertex,
                       const shader::metal::ShaderLibrary* fragment)
          -> std::optional<std::string> {
        // Rasterized and depth tested exactly like the sight. Additive, so
        // that the quads of several sights never erase each other's mask
        // (the target saturates at 1).
        const GraphicsPipelineDesc desc{
            .vertex = &vertex,
            .fragment = fragment,
            .color_format = RenderTargets::kReactiveMaskFormat,
            .depth_format = RenderTargets::kDepthFormat,
            .cull_mode = MTLCullModeNone,
            .depth_compare = MTLCompareFunctionGreaterEqual,
            .depth_write = false,
            .blend = BlendMode::kAdditive,
            .label = "reticle (reactive mask)",
        };
        auto pipeline = Pipeline::Create(context, desc);
        if (!pipeline.ok()) {
          return pipeline.error();
        }
        reactive_pipeline_ = std::move(pipeline).value();
        return std::nullopt;
      });
}

void ReticlePass::Draw(const FrameContext& ctx,
                       id<MTL4RenderCommandEncoder> encoder) const {
  if (!ctx.view.sight) {
    return;
  }
  DrawSight(ctx, encoder, *pipeline_, *ctx.view.sight);
  ++ctx.stats.draw_calls;
}

void ReticlePass::DrawReactive(const FrameContext& ctx,
                               id<MTL4RenderCommandEncoder> encoder) const {
  if (!ctx.view.sight) {
    return;
  }
  // Not counted in the stats: draw_calls are the scene's.
  DrawSight(ctx, encoder, *reactive_pipeline_, *ctx.view.sight);
}

}  // namespace blue::render::metal
