#include "render/metal/passes/shadow_pass.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include <glm/mat4x4.hpp>

#include "camera/frustum.h"
#include "core/path.h"
#include "lighting/cascades.h"
#include "render/metal/encoders.h"
#include "shaders/metal/bindings.h"

namespace blue::render::metal {

namespace {

// Depth bias (setDepthBias:slopeScale:clamp:); tune visually. The constant
// is in units of the smallest resolvable depth difference of kFormat.
constexpr float kDepthBiasConstant = 1.25f;
constexpr float kDepthBiasClamp = 0.0f;
constexpr float kDepthBiasSlope = 1.75f;
// The cascades use standard depth: the far plane is 1.
constexpr double kDepthClearValue = 1.0;

// Must match ShadowConstants in shaders/shadow.vert.metal.
struct ShadowConstants {
  glm::mat4 light_model_view_proj;
};
static_assert(sizeof(ShadowConstants) == 64);

ImageDesc MapDesc() {
  return {
      .extent = {lighting::kShadowMapSize, lighting::kShadowMapSize},
      .format = ShadowPass::kFormat,
      .usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead,
      .array_layers = lighting::kCascadeCount,
      .label = "shadow map",
  };
}

}  // namespace

ShadowPass::ShadowPass(const MetalContext& context, ShaderPrograms& programs,
                       const std::string& shader_dir,
                       const std::vector<Mesh>& meshes)
    : context_(context), meshes_(meshes), map_(context, MapDesc()) {
  programs.Add(
      core::JoinPath(shader_dir, "shadow.vert.metal"), "",
      [this](const shader::metal::ShaderLibrary& vertex,
             const shader::metal::ShaderLibrary* /*fragment*/)
          -> std::optional<std::string> {
        // Depth-only pipeline: no fragment stage, no color.
        const GraphicsPipelineDesc desc{
            .vertex = &vertex,
            .vertex_descriptor = MeshVertexDescriptor(),
            .depth_format = kFormat,
            // Open meshes (teapot rim, ground) must cast from both sides.
            .cull_mode = MTLCullModeNone,
            // Cascades use standard depth (near 0, far 1), unlike the main
            // camera.
            .depth_compare = MTLCompareFunctionLess,
            .depth_clamp = true,
            .depth_bias = true,
            .label = "shadow",
        };
        auto pipeline = Pipeline::Create(context_, desc);
        if (!pipeline.ok()) {
          return pipeline.error();
        }
        pipeline_ = std::move(pipeline).value();
        return std::nullopt;
      });
}

void ShadowPass::Record(const FrameContext& ctx) const {
  if (!ctx.shadows) {
    return;
  }
  constexpr auto kMapSize = static_cast<double>(lighting::kShadowMapSize);

  for (uint32_t layer = 0; layer < ctx.cascades.size(); ++layer) {
    MTL4RenderPassDescriptor* descriptor =
        [[MTL4RenderPassDescriptor alloc] init];
    MTLRenderPassDepthAttachmentDescriptor* depth = descriptor.depthAttachment;
    depth.texture = map_.texture();
    depth.slice = layer;
    depth.loadAction = MTLLoadActionClear;
    depth.storeAction = MTLStoreActionStore;
    depth.clearDepth = kDepthClearValue;

    const id<MTL4RenderCommandEncoder> encoder =
        BeginRenderPass(ctx.rec, descriptor, "shadow cascade");
    [encoder setViewport:MTLViewport{
                             .originX = 0.0,
                             .originY = 0.0,
                             .width = kMapSize,
                             .height = kMapSize,
                             .znear = 0.0,
                             .zfar = 1.0,
                         }];
    pipeline_->Bind(encoder);
    [encoder setDepthBias:kDepthBiasConstant
               slopeScale:kDepthBiasSlope
                    clamp:kDepthBiasClamp];

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
      BindConstants(ctx.rec, kBufferDraw, constants);
      meshes_[mesh_index].Draw(ctx.rec, encoder);
    }
    [encoder endEncoding];
  }
}

}  // namespace blue::render::metal
