#include "render/d3d12/passes/ssao_pass.h"

#include <d3d12.h>

#include <array>
#include <cstdint>
#include <span>

#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include "render/d3d12/image.h"
#include "render/d3d12/render_targets.h"
#include "render/d3d12/samplers.h"
#include "render/shader_manifest.h"

namespace blue::render::d3d12 {

namespace {

// Must match SsaoConstants in shaders/ssao.slang.
struct SsaoConstants {
  glm::vec4 projection;  // x: proj[0][0], y: proj[1][1], z: near plane
  glm::vec4 params;      // x: radius, y: intensity, z: max radius (pixels)
  glm::vec4 full_size;   // xy: full-resolution size, zw: reciprocal
};

// Must match BlurConstants in shaders/ssao_blur.slang.
struct BlurConstants {
  int32_t axis_x;
  int32_t axis_y;
};

template <typename T>
std::span<const std::byte> Bytes(const T& value) {
  return std::as_bytes(std::span(&value, 1));
}

// Records `pass` from `input` into `output` (half resolution).
void Run(ID3D12GraphicsCommandList* cmd, const FullscreenPass& pass,
         Image& input, Image& output, core::Extent2D extent,
         D3D12_GPU_DESCRIPTOR_HANDLE sampler,
         std::span<const std::byte> constants) {
  {
    BarrierBatch barriers(cmd);
    output.Transition(barriers, D3D12_RESOURCE_STATE_RENDER_TARGET);
  }
  const std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 1> inputs = {
      input.staging_srv()};
  pass.Record(cmd, output.rtv(), extent, inputs, sampler, constants);
  BarrierBatch barriers(cmd);
  output.Transition(barriers, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

}  // namespace

SsaoPass::SsaoPass(const D3d12Context& context, ShaderPrograms& programs,
                   const RootSignature& fullscreen_root,
                   const Samplers& samplers)
    : samplers_(samplers),
      occlusion_(context, programs, fullscreen_root, ShaderProgram::kSsao,
                 RenderTargets::kSsaoRawFormat),
      blur_horizontal_(context, programs, fullscreen_root,
                       ShaderProgram::kSsaoBlur, RenderTargets::kSsaoRawFormat),
      blur_vertical_(context, programs, fullscreen_root,
                     ShaderProgram::kSsaoBlur, RenderTargets::kSsaoFormat) {}

void SsaoPass::Record(const FrameContext& ctx) {
  ID3D12GraphicsCommandList* cmd = ctx.cmd;
  RenderTargets& targets = ctx.targets;
  if (!ctx.plan.ssao) {
    if (!target_white_) {
      constexpr float kWhite[4] = {1.0f, 1.0f, 1.0f, 1.0f};
      {
        BarrierBatch barriers(cmd);
        targets.ssao().Transition(barriers, D3D12_RESOURCE_STATE_RENDER_TARGET);
      }
      cmd->ClearRenderTargetView(targets.ssao().rtv(), kWhite, 0, nullptr);
      BarrierBatch barriers(cmd);
      targets.ssao().Transition(barriers,
                                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
      target_white_ = true;
    }
    return;
  }
  target_white_ = false;
  const core::Extent2D full = targets.extent();
  const core::Extent2D half = targets.half_extent();
  const glm::mat4& projection = ctx.view.camera.projection;
  const auto full_width = static_cast<float>(full.width);
  const auto full_height = static_cast<float>(full.height);
  const SsaoConstants constants{
      .projection =
          glm::vec4(projection[0][0], projection[1][1], projection[3][2], 0.0f),
      .params =
          glm::vec4(ctx.view.post.ssao_radius, ctx.view.post.ssao_intensity,
                    kMaxRadiusScreenFraction * full_height, 0.0f),
      .full_size = glm::vec4(full_width, full_height, 1.0f / full_width,
                             1.0f / full_height),
  };
  const D3D12_GPU_DESCRIPTOR_HANDLE nearest = samplers_.nearest_clamp();
  Run(cmd, occlusion_, targets.depth(), targets.ssao_raw(), half, nearest,
      Bytes(constants));
  Run(cmd, blur_horizontal_, targets.ssao_raw(), targets.ssao_blur(), half,
      nearest, Bytes(BlurConstants{.axis_x = 1, .axis_y = 0}));
  Run(cmd, blur_vertical_, targets.ssao_blur(), targets.ssao(), half, nearest,
      Bytes(BlurConstants{.axis_x = 0, .axis_y = 1}));
}

}  // namespace blue::render::d3d12
