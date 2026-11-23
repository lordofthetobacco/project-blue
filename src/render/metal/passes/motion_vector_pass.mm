#include "render/metal/passes/motion_vector_pass.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <glm/mat4x4.hpp>

#include "core/path.h"
#include "render/metal/draw_list.h"
#include "render/metal/encoders.h"
#include "render/metal/material_system.h"
#include "render/metal/passes/scene_pass.h"
#include "shaders/metal/bindings.h"

namespace blue::render::metal {

namespace {

// Must match ObjectMotionConstants in shaders/motion_object.vert.metal. Set
// where the other mesh passes set their model matrix (kBufferDraw).
struct ObjectMotionConstants {
  glm::mat4 model;
  glm::mat4 previous_model;
};
static_assert(offsetof(ObjectMotionConstants, previous_model) == 64);
static_assert(sizeof(ObjectMotionConstants) == 128);

}  // namespace

MotionVectorPass::MotionVectorPass(const MetalContext& context,
                                   ShaderPrograms& programs,
                                   const std::string& shader_dir,
                                   const std::vector<Mesh>& meshes)
    : meshes_(meshes),
      camera_(context, programs, shader_dir,
              {.fragment_path =
                   core::JoinPath(shader_dir, "motion_camera.frag.metal"),
               .output_format = RenderTargets::kMotionVectorFormat,
               .label = "motion vectors camera"}) {
  programs.Add(
      core::JoinPath(shader_dir, "motion_object.vert.metal"),
      core::JoinPath(shader_dir, "motion_object.frag.metal"),
      [this, &context](const shader::metal::ShaderLibrary& vertex,
                       const shader::metal::ShaderLibrary* fragment)
          -> std::optional<std::string> {
        // Both are created before either is replaced, so a failure leaves
        // the previous pair intact.
        std::array<std::unique_ptr<Pipeline>, 2> created;
        for (size_t i = 0; i < created.size(); ++i) {
          const bool double_sided = i == 1;
          const GraphicsPipelineDesc desc{
              .vertex = &vertex,
              .fragment = fragment,
              .vertex_descriptor = MeshVertexDescriptor(),
              .color_format = RenderTargets::kMotionVectorFormat,
              .depth_format = RenderTargets::kDepthFormat,
              .cull_mode = double_sided ? MTLCullModeNone : MTLCullModeBack,
              // Only the surface the depth pre-pass kept, as in the mesh
              // pass (the invariant position matches depth.vert.metal).
              // Alpha-masked cut-outs hold the depth of what is behind
              // them, so they fail the test without a material.
              .depth_compare = MTLCompareFunctionEqual,
              .depth_write = false,
              .label = "motion vectors items",
          };
          auto pipeline = Pipeline::Create(context, desc);
          if (!pipeline.ok()) {
            return pipeline.error();
          }
          created[i] = std::move(pipeline).value();
        }
        item_pipelines_ = std::move(created);
        return std::nullopt;
      });
}

void MotionVectorPass::BindTargets(const RenderTargets& targets,
                                   const Samplers& samplers) {
  if (targets.motion_vectors().texture() == nil) {
    return;  // No temporal scaler: the pass is not recorded.
  }
  // The depth is read texel by texel; the sampler is only what a
  // full-screen input set needs.
  const std::array<id<MTLTexture>, 1> depth{targets.depth().texture()};
  camera_.SetInputs(0, depth, samplers.nearest_clamp());
}

void MotionVectorPass::Record(const FrameContext& ctx) const {
  // The camera step reads the frame's uniforms; a full-screen pass binds
  // only its own inputs.
  ctx.globals.Bind(ctx.rec, ctx.frame_index, ctx.frame_view);
  camera_.Record(ctx.rec, {.texture = ctx.targets.motion_vectors().texture(),
                           .extent = ctx.targets.render_extent()});

  const bool any_moved =
      std::ranges::any_of(ctx.draw_list, [&ctx](const DrawEntry& entry) {
        return ctx.view.draws[entry.item].previous_transform.has_value();
      });
  if (any_moved) {
    RecordItems(ctx);
  }
}

void MotionVectorPass::RecordItems(const FrameContext& ctx) const {
  MTL4RenderPassDescriptor* descriptor =
      [[MTL4RenderPassDescriptor alloc] init];
  MTLRenderPassColorAttachmentDescriptor* color =
      descriptor.colorAttachments[0];
  color.texture = ctx.targets.motion_vectors().texture();
  color.loadAction = MTLLoadActionLoad;  // The camera motion.
  color.storeAction = MTLStoreActionStore;
  MTLRenderPassDepthAttachmentDescriptor* depth = descriptor.depthAttachment;
  depth.texture = ctx.targets.depth().texture();
  depth.loadAction = MTLLoadActionLoad;  // Written by the depth pre-pass.
  // Unchanged (the pipelines do not write depth), but stored: the passes
  // after this one still read it.
  depth.storeAction = MTLStoreActionStore;

  const id<MTL4RenderCommandEncoder> encoder =
      BeginRenderPass(ctx.rec, descriptor, "motion vectors items");
  // Same viewport as the scene pass.
  SetSceneViewport(encoder, ctx.targets.render_extent());
  ctx.globals.Bind(ctx.rec, ctx.frame_index, ctx.frame_view);

  const Pipeline* bound_pipeline = nullptr;
  const Mesh* bound_mesh = nullptr;
  uint32_t set_item = UINT32_MAX;
  for (const DrawEntry& entry : ctx.draw_list) {
    const DrawItem& item = ctx.view.draws[entry.item];
    if (!item.previous_transform) {
      continue;
    }
    // Bindings survive pipeline changes, so the model matrices stay bound
    // across them.
    const Pipeline* pipeline =
        item_pipelines_[IsDoubleSided(entry.variant) ? 1 : 0].get();
    if (pipeline != bound_pipeline) {
      pipeline->Bind(encoder);
      bound_pipeline = pipeline;
    }
    // DrawList::Build() only keeps entries of resident meshes.
    const Mesh& mesh = meshes_[static_cast<size_t>(item.mesh)];
    if (&mesh != bound_mesh) {
      mesh.Bind(ctx.rec);
      bound_mesh = &mesh;
    }
    if (entry.item != set_item) {
      const ObjectMotionConstants constants{
          .model = item.transform,
          .previous_model = *item.previous_transform,
      };
      BindConstants(ctx.rec, kBufferDraw, constants);
      set_item = entry.item;
    }
    mesh.DrawRange(encoder, entry.first_index, entry.index_count);
  }
  [encoder endEncoding];
}

}  // namespace blue::render::metal
