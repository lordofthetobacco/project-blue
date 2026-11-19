#include "render/passes/reticle_pass.h"

#include <cstddef>
#include <optional>
#include <string>
#include <utility>

#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include "core/path.h"
#include "render/render_targets.h"
#include "shaders/bindings.h"

namespace blue::render {

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

}  // namespace

ReticlePass::ReticlePass(const MetalContext& context, ShaderPrograms& programs,
                         const std::string& shader_dir) {
  id<MTLDevice> device = context.device();
  programs.Add(
      core::JoinPath(shader_dir, "reticle.vert.metal"),
      core::JoinPath(shader_dir, "reticle.frag.metal"),
      [this, device](const shader::ShaderLibrary& vertex,
                     const shader::ShaderLibrary* fragment)
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
        auto pipeline = Pipeline::Create(device, desc);
        if (!pipeline.ok()) {
          return pipeline.error();
        }
        pipeline_ = std::move(pipeline).value();
        return std::nullopt;
      });
}

void ReticlePass::Draw(const FrameContext& ctx,
                       id<MTLRenderCommandEncoder> encoder) const {
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
  pipeline_->Bind(encoder);
  [encoder setVertexBytes:&constants
                   length:sizeof(constants)
                  atIndex:kBufferPass];
  [encoder setFragmentBytes:&constants
                     length:sizeof(constants)
                    atIndex:kBufferPass];
  [encoder drawPrimitives:MTLPrimitiveTypeTriangle
              vertexStart:0
              vertexCount:kVertexCount];
  ++ctx.stats.draw_calls;
}

}  // namespace blue::render
