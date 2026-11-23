#include "render/d3d12/passes/motion_blur_pass.h"

#include <d3d12.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include <glm/common.hpp>
#include <glm/ext/vector_int4.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>

#include "camera/frustum.h"
#include "render/binding_layout.h"
#include "render/d3d12/d3d12_context.h"
#include "render/d3d12/image.h"
#include "render/d3d12/pipeline.h"
#include "render/d3d12/render_targets.h"
#include "render/d3d12/root_layout.h"
#include "render/d3d12/samplers.h"
#include "render/draw_list.h"
#include "render/motion_blur_math.h"
#include "render/shader_manifest.h"

namespace blue::render::d3d12 {

namespace {

// Must match VelocityConstants in shaders/motion_velocity.slang.
struct VelocityConstants {
  glm::mat4 model;
  glm::mat4 previous_model;
};
static_assert(sizeof(VelocityConstants) <= kMaxPushConstantBytes);

// Must match TileMaxConstants in shaders/motion_tile_max.slang.
struct TileMaxConstants {
  glm::ivec4 area;  // xy: min pixel, zw: max pixel (exclusive)
  int32_t tile_size;
  float max_length;
};

// Must match BlurConstants in shaders/motion_blur_gather.slang.
struct BlurConstants {
  float max_length;
  int32_t tile_size;
};

template <typename T>
std::span<const std::byte> Bytes(const T& value) {
  return std::as_bytes(std::span(&value, 1));
}

}  // namespace

MotionBlurPass::MotionBlurPass(const D3d12Context& context,
                               ShaderPrograms& programs,
                               const RootSignature& scene_root,
                               const RootSignature& fullscreen_root,
                               const FrameGlobals& frame_globals,
                               const Samplers& samplers,
                               const std::vector<Mesh>& meshes)
    : context_(context),
      scene_root_(scene_root),
      params_(SceneRootParams::Of(scene_root.layout(), {})),
      frame_globals_(frame_globals),
      samplers_(samplers),
      meshes_(meshes),
      tile_max_(context, programs, fullscreen_root,
                ShaderProgram::kMotionTileMax,
                RenderTargets::kMotionTileFormat),
      blur_(context, programs, fullscreen_root, ShaderProgram::kMotionBlur,
            RenderTargets::kHdrFormat) {
  programs.Add(
      ShaderProgram::kMotionVelocity, [this](std::span<const std::byte> vertex,
                                             std::span<const std::byte> pixel) {
        for (size_t i = 0; i < velocity_pipelines_.size(); ++i) {
          GraphicsPipelineDesc desc;
          desc.root_signature = scene_root_.get();
          desc.vertex = vertex;
          desc.pixel = pixel;
          desc.input_layout = VertexInputLayout();
          desc.rtv_formats[0] = RenderTargets::kVelocityFormat;
          desc.rtv_count = 1;
          desc.dsv_format = RenderTargets::kDepthFormat;
          desc.cull = i == 1 ? D3D12_CULL_MODE_NONE : D3D12_CULL_MODE_BACK;
          // Only the surface the depth pre-pass kept, as in the
          // mesh pass.
          desc.depth_test = true;
          desc.depth_write = false;
          desc.depth_func = D3D12_COMPARISON_FUNC_EQUAL;
          desc.name = L"blue motion velocity pipeline";
          velocity_pipelines_[i] =
              CreateGraphicsPipeline(context_.device(), desc);
        }
      });
}

std::optional<D3D12_RECT> MotionBlurPass::FindArea(const FrameContext& ctx) {
  const PostSettings& post = ctx.view.post;
  if (!post.weapon_motion_blur_enabled ||
      MotionBlurScale(post.weapon_motion_blur_strength,
                      ctx.view.frame_seconds) <= 0.0f) {
    return std::nullopt;
  }
  const core::Extent2D extent = ctx.targets.extent();
  const glm::vec2 size(static_cast<float>(extent.width),
                       static_cast<float>(extent.height));
  const glm::mat4 view_proj = ctx.view.camera.projection * ctx.view.camera.view;
  std::optional<ScreenBounds> bounds;
  uint32_t added_item = UINT32_MAX;
  for (const DrawEntry& entry : ctx.draw_list) {
    const DrawItem& item = ctx.view.draws[entry.item];
    if (!item.previous_transform || !item.motion_blur ||
        entry.item == added_item) {
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
    return D3D12_RECT{0, 0, static_cast<LONG>(extent.width),
                      static_cast<LONG>(extent.height)};
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
  return D3D12_RECT{static_cast<LONG>(low.x), static_cast<LONG>(low.y),
                    static_cast<LONG>(high.x), static_cast<LONG>(high.y)};
}

void MotionBlurPass::Record(const FrameContext& ctx,
                            const D3D12_RECT& area) const {
  RecordVelocity(ctx);
  RecordTileMax(ctx, area);
  RecordSourceCopy(ctx, area);

  RenderTargets& targets = ctx.targets;
  const BlurConstants constants{
      .max_length = 2.0f * static_cast<float>(targets.motion_tile_size()),
      .tile_size = targets.motion_tile_size(),
  };
  // Every input is fetched texel by texel.
  const std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 3> inputs = {
      targets.motion_source().staging_srv(), targets.velocity().staging_srv(),
      targets.motion_tiles().staging_srv()};
  blur_.Record(ctx.cmd, targets.hdr().rtv(), targets.extent(), area, inputs,
               samplers_.nearest_clamp(), Bytes(constants));
  BarrierBatch barriers(ctx.cmd);
  targets.hdr().Transition(barriers,
                           D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

void MotionBlurPass::RecordVelocity(const FrameContext& ctx) const {
  ID3D12GraphicsCommandList* cmd = ctx.cmd;
  Image& velocity = ctx.targets.velocity();
  Image& depth = ctx.targets.depth();
  {
    BarrierBatch barriers(cmd);
    velocity.Transition(barriers, D3D12_RESOURCE_STATE_RENDER_TARGET);
    depth.Transition(barriers, RenderTargets::kDepthReadState);
  }
  const D3D12_CPU_DESCRIPTOR_HANDLE rtv = velocity.rtv();
  const D3D12_CPU_DESCRIPTOR_HANDLE dsv = depth.read_only_dsv();
  cmd->ClearRenderTargetView(rtv, RenderTargets::kNoMotion, 0, nullptr);
  cmd->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
  SetViewport(cmd, ctx.targets.extent());
  cmd->SetGraphicsRootSignature(scene_root_.get());
  frame_globals_.Bind(cmd, params_, ctx.frame);
  cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

  ID3D12PipelineState* bound_pipeline = nullptr;
  const Mesh* bound_mesh = nullptr;
  uint32_t pushed_item = UINT32_MAX;
  for (const DrawEntry& entry : ctx.draw_list) {
    const DrawItem& item = ctx.view.draws[entry.item];
    if (!item.previous_transform || !item.motion_blur) {
      continue;
    }
    ID3D12PipelineState* pipeline =
        velocity_pipelines_[IsDoubleSided(entry.variant) ? 1 : 0].Get();
    if (pipeline != bound_pipeline) {
      cmd->SetPipelineState(pipeline);
      bound_pipeline = pipeline;
    }
    // DrawList::Build() only keeps entries of resident meshes.
    const Mesh& mesh = meshes_[static_cast<size_t>(item.mesh)];
    if (&mesh != bound_mesh) {
      mesh.Bind(cmd);
      bound_mesh = &mesh;
    }
    if (entry.item != pushed_item) {
      const VelocityConstants constants{
          .model = item.transform,
          .previous_model = *item.previous_transform,
      };
      cmd->SetGraphicsRoot32BitConstants(kPushConstantsParam,
                                         sizeof(constants) / 4, &constants, 0);
      pushed_item = entry.item;
    }
    Mesh::DrawRange(cmd, entry.first_index, entry.index_count);
  }

  BarrierBatch barriers(cmd);
  velocity.Transition(barriers, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

void MotionBlurPass::RecordTileMax(const FrameContext& ctx,
                                   const D3D12_RECT& area) const {
  Image& tiles = ctx.targets.motion_tiles();
  {
    BarrierBatch barriers(ctx.cmd);
    tiles.Transition(barriers, D3D12_RESOURCE_STATE_RENDER_TARGET);
  }
  // Every tile is written (those outside `area` with zero, cheaply), so the
  // blur's 3 x 3 neighbourhood never reads a stale one.
  const TileMaxConstants constants{
      .area = glm::ivec4(area.left, area.top, area.right, area.bottom),
      .tile_size = ctx.targets.motion_tile_size(),
      .max_length = 2.0f * static_cast<float>(ctx.targets.motion_tile_size()),
  };
  const std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 1> inputs = {
      ctx.targets.velocity().staging_srv()};
  tile_max_.Record(ctx.cmd, tiles.rtv(), tiles.extent(), inputs,
                   samplers_.nearest_clamp(), Bytes(constants));
  BarrierBatch barriers(ctx.cmd);
  tiles.Transition(barriers, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

void MotionBlurPass::RecordSourceCopy(const FrameContext& ctx,
                                      const D3D12_RECT& area) {
  ID3D12GraphicsCommandList* cmd = ctx.cmd;
  Image& hdr = ctx.targets.hdr();
  Image& source = ctx.targets.motion_source();
  {
    BarrierBatch barriers(cmd);
    hdr.Transition(barriers, D3D12_RESOURCE_STATE_COPY_SOURCE);
    source.Transition(barriers, D3D12_RESOURCE_STATE_COPY_DEST);
  }
  const D3D12_TEXTURE_COPY_LOCATION destination{
      .pResource = source.resource(),
      .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
      .SubresourceIndex = 0,
  };
  const D3D12_TEXTURE_COPY_LOCATION copy_source{
      .pResource = hdr.resource(),
      .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
      .SubresourceIndex = 0,
  };
  const D3D12_BOX box{
      static_cast<UINT>(area.left),  static_cast<UINT>(area.top),    0,
      static_cast<UINT>(area.right), static_cast<UINT>(area.bottom), 1};
  cmd->CopyTextureRegion(&destination, static_cast<UINT>(area.left),
                         static_cast<UINT>(area.top), 0, &copy_source, &box);
  // The blur rewrites `area` of hdr and keeps the rest.
  BarrierBatch barriers(cmd);
  source.Transition(barriers, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  hdr.Transition(barriers, D3D12_RESOURCE_STATE_RENDER_TARGET);
}

}  // namespace blue::render::d3d12
