#include "render/d3d12/passes/tonemap_pass.h"

#include <d3d12.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <span>

#include "render/d3d12/image.h"
#include "render/d3d12/render_targets.h"
#include "render/d3d12/samplers.h"
#include "render/shader_manifest.h"

namespace blue::render::d3d12 {

namespace {

// Must match TonemapConstants in shaders/tonemap.slang.
struct TonemapConstants {
  float exposure_scale;  // 2^EV
  int32_t tonemapper;    // Tonemapper
  float bloom_strength;  // 0 when bloom is off: the bloom image is not read
  float bloom_scale;     // 1 / bloom levels: mip 0 holds their sum
};

}  // namespace

TonemapPass::TonemapPass(const D3d12Context& context, ShaderPrograms& programs,
                         const RootSignature& fullscreen_root,
                         const Samplers& samplers)
    : samplers_(samplers),
      pass_(context, programs, fullscreen_root, ShaderProgram::kTonemap,
            RenderTargets::kLdrFormat) {}

void TonemapPass::Record(const FrameContext& ctx) const {
  Image& ldr = ctx.targets.ldr();
  {
    BarrierBatch barriers(ctx.cmd);
    ctx.targets.hdr().Transition(barriers,
                                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    ldr.Transition(barriers, D3D12_RESOURCE_STATE_RENDER_TARGET);
  }
  // No bloom pass yet (package D3): the bloom input is a null view and its
  // strength 0, so the shader never reads it.
  const TonemapConstants constants{
      .exposure_scale = std::exp2(ctx.view.post.exposure_ev),
      .tonemapper = static_cast<int32_t>(ctx.view.post.tonemapper),
      .bloom_strength = 0.0f,
      .bloom_scale = 1.0f,
  };
  const std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 1> inputs = {
      ctx.targets.hdr().staging_srv()};
  pass_.Record(ctx.cmd, ldr.rtv(), ctx.targets.extent(), inputs,
               samplers_.linear_clamp(),
               std::as_bytes(std::span(&constants, 1)));
  BarrierBatch barriers(ctx.cmd);
  ldr.Transition(barriers, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

}  // namespace blue::render::d3d12
