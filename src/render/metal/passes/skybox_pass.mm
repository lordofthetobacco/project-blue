#include "render/metal/passes/skybox_pass.h"

#include <optional>
#include <string>
#include <utility>

#include "core/path.h"
#include "render/metal/encoders.h"
#include "render/metal/render_targets.h"
#include "shaders/metal/bindings.h"

namespace blue::render::metal {

namespace {

// The vertex shader emits one triangle that covers the whole viewport.
constexpr NSUInteger kVertexCount = 3;

}  // namespace

SkyboxPass::SkyboxPass(const MetalContext& context, ShaderPrograms& programs,
                       const std::string& shader_dir,
                       const Environment& environment)
    : environment_(environment) {
  programs.Add(
      core::JoinPath(shader_dir, "skybox.vert.metal"),
      core::JoinPath(shader_dir, "skybox.frag.metal"),
      [this, &context](const shader::metal::ShaderLibrary& vertex,
                     const shader::metal::ShaderLibrary* fragment)
          -> std::optional<std::string> {
        // Only where nothing was drawn: the depth there is still the clear
        // value 0, which the triangle at depth 0 equals.
        const GraphicsPipelineDesc desc{
            .vertex = &vertex,
            .fragment = fragment,
            .color_format = RenderTargets::kHdrFormat,
            .depth_format = RenderTargets::kDepthFormat,
            .cull_mode = MTLCullModeNone,
            .depth_compare = MTLCompareFunctionGreaterEqual,
            .depth_write = false,
            .label = "skybox",
        };
        auto pipeline = Pipeline::Create(context, desc);
        if (!pipeline.ok()) {
          return pipeline.error();
        }
        pipeline_ = std::move(pipeline).value();
        return std::nullopt;
      });
}

void SkyboxPass::Draw(const FrameContext& ctx,
                      id<MTL4RenderCommandEncoder> encoder) const {
  pipeline_->Bind(encoder);
  BindTexture(ctx.rec, kTextureSky, environment_.sky_texture());
  BindSampler(ctx.rec, kSamplerLinearClamp, environment_.sky_sampler());
  [encoder drawPrimitives:MTLPrimitiveTypeTriangle
              vertexStart:0
              vertexCount:kVertexCount];
  ++ctx.stats.draw_calls;
}

}  // namespace blue::render::metal
