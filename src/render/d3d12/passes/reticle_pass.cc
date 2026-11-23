#include "render/d3d12/passes/reticle_pass.h"

#include <d3d12.h>

#include <cstddef>
#include <cstdint>
#include <span>

#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include "render/binding_layout.h"
#include "render/d3d12/d3d12_context.h"
#include "render/d3d12/pipeline.h"
#include "render/d3d12/render_targets.h"
#include "render/d3d12/root_layout.h"
#include "render/shader_manifest.h"

namespace blue::render::d3d12 {

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
static_assert(sizeof(ReticleConstants) <= kMaxPushConstantBytes);

// The sight quad is generated in the vertex shader from two triangles.
constexpr UINT kVertexCount = 6;

}  // namespace

ReticlePass::ReticlePass(const D3d12Context& context, ShaderPrograms& programs,
                         const RootSignature& root_signature,
                         const FrameGlobals& frame_globals)
    : context_(context),
      root_signature_(root_signature),
      params_(SceneRootParams::Of(root_signature.layout(), {})),
      frame_globals_(frame_globals) {
  programs.Add(ShaderProgram::kReticle,
               [this](std::span<const std::byte> vertex,
                      std::span<const std::byte> pixel) {
                 // Additive, depth tested against the scene (equal passes too,
                 // for a window lying on the model's own glass) without writing
                 // depth, and visible from both sides.
                 GraphicsPipelineDesc desc;
                 desc.root_signature = root_signature_.get();
                 desc.vertex = vertex;
                 desc.pixel = pixel;
                 desc.rtv_formats[0] = RenderTargets::kHdrFormat;
                 desc.rtv_count = 1;
                 desc.dsv_format = RenderTargets::kDepthFormat;
                 desc.depth_test = true;
                 desc.depth_write = false;
                 desc.depth_func = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
                 desc.blend = BlendDesc(BlendMode::kAdditive);
                 desc.name = L"blue reticle pipeline";
                 pipeline_ = CreateGraphicsPipeline(context_.device(), desc);
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
  ID3D12GraphicsCommandList* cmd = ctx.cmd;
  cmd->SetGraphicsRootSignature(root_signature_.get());
  cmd->SetPipelineState(pipeline_.Get());
  frame_globals_.Bind(cmd, params_, ctx.frame);
  cmd->SetGraphicsRoot32BitConstants(kPushConstantsParam, sizeof(constants) / 4,
                                     &constants, 0);
  cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  cmd->DrawInstanced(kVertexCount, 1, 0, 0);
  ++ctx.stats.draw_calls;
}

}  // namespace blue::render::d3d12
