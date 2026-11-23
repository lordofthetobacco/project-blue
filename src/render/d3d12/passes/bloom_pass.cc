#include "render/d3d12/passes/bloom_pass.h"

#include <d3d12.h>

#include <array>
#include <cstdint>
#include <span>

#include "render/d3d12/image.h"
#include "render/d3d12/pipeline.h"
#include "render/d3d12/render_targets.h"
#include "render/d3d12/samplers.h"
#include "render/shader_manifest.h"

namespace blue::render::d3d12 {

namespace {

// Must match DownsampleConstants in shaders/bloom_down.slang.
struct DownsampleConstants {
  float source_texel_x;
  float source_texel_y;
  int32_t karis_average;
  int32_t padding;
};

// Must match UpsampleConstants in shaders/bloom_up.slang.
struct UpsampleConstants {
  float source_texel_x;
  float source_texel_y;
};

template <typename T>
std::span<const std::byte> Bytes(const T& value) {
  return std::as_bytes(std::span(&value, 1));
}

// Brings `image` into `state`.
void Move(ID3D12GraphicsCommandList* cmd, Image& image,
          D3D12_RESOURCE_STATES state) {
  BarrierBatch barriers(cmd);
  image.Transition(barriers, state);
}

}  // namespace

BloomPass::BloomPass(const D3d12Context& context, ShaderPrograms& programs,
                     const RootSignature& fullscreen_root,
                     const Samplers& samplers)
    : samplers_(samplers),
      downsample_(context, programs, fullscreen_root, ShaderProgram::kBloomDown,
                  RenderTargets::kBloomFormat),
      upsample_(context, programs, fullscreen_root, ShaderProgram::kBloomUp,
                RenderTargets::kBloomFormat, BlendMode::kAdditive) {}

void BloomPass::Record(const FrameContext& ctx) const {
  ID3D12GraphicsCommandList* cmd = ctx.cmd;
  RenderTargets& targets = ctx.targets;
  const uint32_t levels = targets.bloom_levels();
  constexpr D3D12_RESOURCE_STATES kRead =
      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  constexpr D3D12_RESOURCE_STATES kWrite = D3D12_RESOURCE_STATE_RENDER_TARGET;
  const D3D12_GPU_DESCRIPTOR_HANDLE linear = samplers_.linear_clamp();

  for (uint32_t level = 0; level < levels; ++level) {
    Image& bloom = targets.bloom(level);
    Move(cmd, bloom, kWrite);
    const core::Extent2D source =
        level == 0 ? targets.extent() : targets.bloom_mip_extent(level - 1);
    const DownsampleConstants constants{
        .source_texel_x = 1.0f / static_cast<float>(source.width),
        .source_texel_y = 1.0f / static_cast<float>(source.height),
        .karis_average = level == 0 ? 1 : 0,
        .padding = 0,
    };
    const std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 1> inputs = {
        level == 0 ? targets.hdr().staging_srv()
                   : targets.bloom(level - 1).staging_srv()};
    downsample_.Record(cmd, bloom.rtv(), targets.bloom_mip_extent(level),
                       inputs, linear, Bytes(constants));
    Move(cmd, bloom, kRead);
  }

  for (uint32_t level = levels - 1; level > 0; --level) {
    const uint32_t target = level - 1;
    // Keeps the downsampled image: the upsample adds onto it.
    Image& bloom = targets.bloom(target);
    Move(cmd, bloom, kWrite);
    const core::Extent2D source = targets.bloom_mip_extent(level);
    const UpsampleConstants constants{
        .source_texel_x = 1.0f / static_cast<float>(source.width),
        .source_texel_y = 1.0f / static_cast<float>(source.height),
    };
    const std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 1> inputs = {
        targets.bloom(level).staging_srv()};
    upsample_.Record(cmd, bloom.rtv(), targets.bloom_mip_extent(target), inputs,
                     linear, Bytes(constants));
    Move(cmd, bloom, kRead);
  }
}

}  // namespace blue::render::d3d12
