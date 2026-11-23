#include "render/metal/passes/depth_prepass.h"

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "core/path.h"
#include "render/metal/draw_list.h"
#include "render/metal/encoders.h"
#include "render/metal/passes/scene_pass.h"
#include "render/metal/render_targets.h"

namespace blue::render::metal {

namespace {

// Reversed-Z: infinity is 0.
constexpr double kDepthClearValue = 0.0;

}  // namespace

DepthPrepass::DepthPrepass(const MetalContext& context,
                           ShaderPrograms& programs,
                           const std::string& shader_dir,
                           const MaterialSystem& materials,
                           const std::vector<Mesh>& meshes)
    : context_(context), materials_(materials), meshes_(meshes) {
  const std::string vertex_path =
      core::JoinPath(shader_dir, "depth.vert.metal");
  programs.Add(vertex_path, "",
               [this](const shader::metal::ShaderLibrary& vertex,
                      const shader::metal::ShaderLibrary* fragment) {
                 return CreatePipelines(/*masked=*/false, vertex, fragment);
               });
  programs.Add(vertex_path,
               core::JoinPath(shader_dir, "depth_masked.frag.metal"),
               [this](const shader::metal::ShaderLibrary& vertex,
                      const shader::metal::ShaderLibrary* fragment) {
                 return CreatePipelines(/*masked=*/true, vertex, fragment);
               });
}

std::optional<std::string> DepthPrepass::CreatePipelines(
    bool masked, const shader::metal::ShaderLibrary& vertex,
    const shader::metal::ShaderLibrary* fragment) {
  // All of them are created before any is replaced, so a failure leaves the
  // previous set intact.
  std::array<std::unique_ptr<Pipeline>, kMaterialVariantCount> created;
  for (size_t i = 0; i < kMaterialVariantCount; ++i) {
    const auto variant = static_cast<MaterialVariant>(i);
    if (IsMasked(variant) != masked) {
      continue;
    }
    const GraphicsPipelineDesc desc{
        .vertex = &vertex,
        .fragment = fragment,
        .vertex_descriptor = MeshVertexDescriptor(),
        .depth_format = RenderTargets::kDepthFormat,
        .cull_mode =
            IsDoubleSided(variant) ? MTLCullModeNone : MTLCullModeBack,
        .depth_compare = MTLCompareFunctionGreater,
        .label = masked ? "depth prepass (masked)" : "depth prepass",
    };
    auto pipeline = Pipeline::Create(context_, desc);
    if (!pipeline.ok()) {
      return pipeline.error();
    }
    created[i] = std::move(pipeline).value();
  }
  for (size_t i = 0; i < kMaterialVariantCount; ++i) {
    if (created[i] != nullptr) {
      pipelines_[i] = std::move(created[i]);
    }
  }
  return std::nullopt;
}

void DepthPrepass::Record(const FrameContext& ctx) const {
  MTL4RenderPassDescriptor* descriptor =
      [[MTL4RenderPassDescriptor alloc] init];
  MTLRenderPassDepthAttachmentDescriptor* depth = descriptor.depthAttachment;
  depth.texture = ctx.scene_depth.texture();
  depth.loadAction = MTLLoadActionClear;
  depth.storeAction = MTLStoreActionStore;
  depth.clearDepth = kDepthClearValue;

  const id<MTL4RenderCommandEncoder> encoder =
      BeginRenderPass(ctx.rec, descriptor, "depth prepass");
  SetSceneViewport(encoder, ctx.scene_extent);
  ctx.globals.Bind(ctx.rec, ctx.frame_index, ctx.frame_view);

  DrawListPipelines pipelines;
  for (size_t i = 0; i < kMaterialVariantCount; ++i) {
    pipelines.pipelines[i] = pipelines_[i].get();
    pipelines.bind_material[i] = IsMasked(static_cast<MaterialVariant>(i));
  }
  RecordDrawList(ctx.rec, encoder, ctx.draw_list, pipelines, ctx.view,
                 materials_, meshes_, /*stats=*/nullptr);
  [encoder endEncoding];
}

}  // namespace blue::render::metal
