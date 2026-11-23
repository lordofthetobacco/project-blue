#include "render/metal/passes/motion_blur_pass.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <glm/common.hpp>
#include <glm/ext/vector_int4.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>

#include "camera/frustum.h"
#include "core/extent.h"
#include "core/path.h"
#include "render/metal/draw_list.h"
#include "render/metal/encoders.h"
#include "render/metal/material_system.h"
#include "render/motion_blur_math.h"
#include "render/metal/passes/scene_pass.h"
#include "shaders/metal/bindings.h"

namespace blue::render::metal {

namespace {

// Must match VelocityConstants in shaders/motion_velocity.vert.metal. Set
// where the other mesh passes set their model matrix (kBufferDraw).
struct VelocityConstants {
  glm::mat4 model;
  glm::mat4 previous_model;
};
static_assert(offsetof(VelocityConstants, previous_model) == 64);
static_assert(sizeof(VelocityConstants) == 128);

// Must match TileMaxConstants in shaders/motion_tile_max.frag.metal, whose
// int4 rounds the size up to a multiple of 16 bytes.
struct TileMaxConstants {
  glm::ivec4 area;  // xy: min pixel, zw: max pixel (exclusive)
  int32_t tile_size;
  float max_length;
  std::array<int32_t, 2> padding{};
};
static_assert(offsetof(TileMaxConstants, tile_size) == 16);
static_assert(offsetof(TileMaxConstants, max_length) == 20);
static_assert(sizeof(TileMaxConstants) == 32);

// Must match BlurConstants in shaders/motion_blur.frag.metal.
struct BlurConstants {
  float max_length;
  int32_t tile_size;
};
static_assert(sizeof(BlurConstants) == 8);

// "No motion": a zero blur vector (0.5 in the encoding) and no coverage
// (shaders/motion_blur.h, EncodeBlur).
constexpr MTLClearColor kNoMotion{0.5, 0.5, 0.0, 0.0};

// Returns true for the items the pass blurs: those that moved and ask for
// it (the first-person weapon, not what merely moves in the world).
bool IsBlurred(const DrawItem& item) {
  return item.previous_transform.has_value() && item.motion_blur;
}

}  // namespace

MotionBlurPass::MotionBlurPass(const MetalContext& context,
                               ShaderPrograms& programs,
                               const std::string& shader_dir,
                               const std::vector<Mesh>& meshes)
    : meshes_(meshes),
      tile_max_(context, programs, shader_dir,
                {.fragment_path =
                     core::JoinPath(shader_dir, "motion_tile_max.frag.metal"),
                 .pass_constant_size = sizeof(TileMaxConstants),
                 .output_format = RenderTargets::kMotionTileFormat,
                 .label = "motion blur tile max"}),
      blur_(context, programs, shader_dir,
            {.fragment_path =
                 core::JoinPath(shader_dir, "motion_blur.frag.metal"),
             .input_count = 3,
             .pass_constant_size = sizeof(BlurConstants),
             .output_format = RenderTargets::kHdrFormat,
             .label = "motion blur"}) {
  programs.Add(
      core::JoinPath(shader_dir, "motion_velocity.vert.metal"),
      core::JoinPath(shader_dir, "motion_velocity.frag.metal"),
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
              .color_format = RenderTargets::kVelocityFormat,
              .depth_format = RenderTargets::kDepthFormat,
              .cull_mode = double_sided ? MTLCullModeNone : MTLCullModeBack,
              // Only the surface the depth pre-pass kept, as in the mesh
              // pass (the invariant position matches depth.vert.metal).
              // Alpha-masked cut-outs hold the depth of what is behind
              // them, so they fail the test without a material.
              .depth_compare = MTLCompareFunctionEqual,
              .depth_write = false,
              .label = "motion blur velocity",
          };
          auto pipeline = Pipeline::Create(context, desc);
          if (!pipeline.ok()) {
            return pipeline.error();
          }
          created[i] = std::move(pipeline).value();
        }
        velocity_pipelines_ = std::move(created);
        return std::nullopt;
      });
}

void MotionBlurPass::BindTargets(const RenderTargets& targets,
                                 const Samplers& samplers) {
  // Every input is read texel by texel; the sampler is only what a
  // full-screen input set needs.
  const std::array<id<MTLTexture>, 1> velocity{targets.velocity().texture()};
  tile_max_.SetInputs(0, velocity, samplers.nearest_clamp());
  const std::array<id<MTLTexture>, 3> blur_inputs{
      targets.motion_source().texture(), targets.velocity().texture(),
      targets.motion_tiles().texture()};
  blur_.SetInputs(0, blur_inputs, samplers.nearest_clamp());
}

std::optional<MTLScissorRect> MotionBlurPass::FindArea(
    const FrameContext& ctx) {
  const PostSettings& post = ctx.view.post;
  if (!post.weapon_motion_blur_enabled ||
      MotionBlurScale(post.weapon_motion_blur_strength,
                      ctx.view.frame_seconds) <= 0.0f) {
    return std::nullopt;
  }
  const core::Extent2D extent = ctx.targets.render_extent();
  const glm::vec2 size(static_cast<float>(extent.width),
                       static_cast<float>(extent.height));
  const glm::mat4 view_proj = ctx.view.camera.projection * ctx.view.camera.view;
  // Every covered pixel lies inside `bounds`: the blur relies on it (taps
  // outside the copied area may only be background taps).
  std::optional<ScreenBounds> bounds;
  uint32_t added_item = UINT32_MAX;
  for (const DrawEntry& entry : ctx.draw_list) {
    if (!IsBlurred(ctx.view.draws[entry.item]) || entry.item == added_item) {
      continue;
    }
    added_item = entry.item;
    const camera::Aabb& box = ctx.world_bounds[entry.item];
    const std::optional<ScreenBounds> item_bounds =
        ProjectBoxToScreen(box.min, box.max, view_proj, size);
    if (!item_bounds) {
      continue;  // Nothing of it can be rasterized.
    }
    bounds = bounds ? ScreenBounds{glm::min(bounds->min, item_bounds->min),
                                   glm::max(bounds->max, item_bounds->max)}
                    : *item_bounds;
  }
  if (!bounds) {
    return std::nullopt;
  }
  if (!std::isfinite(bounds->min.x) || !std::isfinite(bounds->min.y) ||
      !std::isfinite(bounds->max.x) || !std::isfinite(bounds->max.y)) {
    // Degenerate matrices.
    return MTLScissorRect{0, 0, extent.width, extent.height};
  }
  // Blur reaches at most the largest radius past the items' pixels; one more
  // pixel covers rounding.
  const float margin =
      static_cast<float>(ctx.targets.motion_tile_size()) + 1.0f;
  const glm::vec2 low =
      glm::clamp(glm::floor(bounds->min - margin), glm::vec2(0.0f), size);
  const glm::vec2 high =
      glm::clamp(glm::ceil(bounds->max + margin), glm::vec2(0.0f), size);
  if (!(high.x > low.x) || !(high.y > low.y)) {
    return std::nullopt;  // Entirely off screen.
  }
  return MTLScissorRect{
      .x = static_cast<NSUInteger>(low.x),
      .y = static_cast<NSUInteger>(low.y),
      .width = static_cast<NSUInteger>(high.x - low.x),
      .height = static_cast<NSUInteger>(high.y - low.y),
  };
}

void MotionBlurPass::Record(const FrameContext& ctx,
                            const MTLScissorRect& area) const {
  RecordVelocity(ctx);
  RecordTileMax(ctx, area);
  RecordSourceCopy(ctx, area);

  const RenderTargets& targets = ctx.targets;
  const BlurConstants constants{
      .max_length = 2.0f * static_cast<float>(targets.motion_tile_size()),
      .tile_size = targets.motion_tile_size(),
  };
  // The scissor keeps the HDR image outside `area` as the scene pass left
  // it.
  blur_.Record(ctx.rec,
               {.texture = targets.hdr().texture(),
                .extent = targets.render_extent(),
                .load_action = MTLLoadActionLoad,
                .area = area},
               0, ConstantBytes(constants));
}

void MotionBlurPass::RecordVelocity(const FrameContext& ctx) const {
  MTL4RenderPassDescriptor* descriptor =
      [[MTL4RenderPassDescriptor alloc] init];
  MTLRenderPassColorAttachmentDescriptor* color =
      descriptor.colorAttachments[0];
  color.texture = ctx.targets.velocity().texture();
  color.loadAction = MTLLoadActionClear;
  color.storeAction = MTLStoreActionStore;
  color.clearColor = kNoMotion;
  MTLRenderPassDepthAttachmentDescriptor* depth = descriptor.depthAttachment;
  depth.texture = ctx.targets.depth().texture();
  depth.loadAction = MTLLoadActionLoad;  // Written by the depth pre-pass.
  // Unchanged (the pipelines do not write depth), but stored: the sights
  // drawn after the blur still test against it.
  depth.storeAction = MTLStoreActionStore;

  const id<MTL4RenderCommandEncoder> encoder =
      BeginRenderPass(ctx.rec, descriptor, "motion blur velocity");
  // Same viewport as the scene pass.
  SetSceneViewport(encoder, ctx.targets.render_extent());
  ctx.globals.Bind(ctx.rec, ctx.frame_index, ctx.frame_view);

  const Pipeline* bound_pipeline = nullptr;
  const Mesh* bound_mesh = nullptr;
  uint32_t set_item = UINT32_MAX;
  for (const DrawEntry& entry : ctx.draw_list) {
    const DrawItem& item = ctx.view.draws[entry.item];
    if (!IsBlurred(item)) {
      continue;
    }
    // Bindings survive pipeline changes, so the model matrices stay bound
    // across them.
    const Pipeline* pipeline =
        velocity_pipelines_[IsDoubleSided(entry.variant) ? 1 : 0].get();
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
      const VelocityConstants constants{
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

void MotionBlurPass::RecordTileMax(const FrameContext& ctx,
                                   const MTLScissorRect& area) const {
  const Image& tiles = ctx.targets.motion_tiles();
  // Every tile is written (those outside `area` with zero, cheaply), so the
  // blur's 3 x 3 neighbourhood never reads a stale one.
  const TileMaxConstants constants{
      .area = glm::ivec4(static_cast<int32_t>(area.x),
                         static_cast<int32_t>(area.y),
                         static_cast<int32_t>(area.x + area.width),
                         static_cast<int32_t>(area.y + area.height)),
      .tile_size = ctx.targets.motion_tile_size(),
      .max_length = 2.0f * static_cast<float>(ctx.targets.motion_tile_size()),
  };
  tile_max_.Record(ctx.rec,
                   {.texture = tiles.texture(), .extent = tiles.extent()}, 0,
                   ConstantBytes(constants));
}

void MotionBlurPass::RecordSourceCopy(const FrameContext& ctx,
                                      const MTLScissorRect& area) {
  // What is outside `area` of motion_source is never read.
  const id<MTL4ComputeCommandEncoder> copy =
      BeginCopy(ctx.rec, "motion blur source copy");
  const MTLOrigin origin = MTLOriginMake(area.x, area.y, 0);
  [copy copyFromTexture:ctx.targets.hdr().texture()
            sourceSlice:0
            sourceLevel:0
           sourceOrigin:origin
             sourceSize:MTLSizeMake(area.width, area.height, 1)
              toTexture:ctx.targets.motion_source().texture()
       destinationSlice:0
       destinationLevel:0
      destinationOrigin:origin];
  [copy endEncoding];
}

}  // namespace blue::render::metal
