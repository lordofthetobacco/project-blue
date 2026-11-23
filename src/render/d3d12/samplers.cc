#include "render/d3d12/samplers.h"

#include <d3d12.h>

#include <algorithm>
#include <cmath>

#include "render/d3d12/d3d12_context.h"
#include "render/d3d12/descriptor_heaps.h"

namespace blue::render::d3d12 {

namespace {

// Returns a sampler with `filter`, `address` on every axis and every mip
// reachable.
D3D12_SAMPLER_DESC SamplerDesc(D3D12_FILTER filter,
                               D3D12_TEXTURE_ADDRESS_MODE address) {
  return {
      .Filter = filter,
      .AddressU = address,
      .AddressV = address,
      .AddressW = address,
      .MipLODBias = 0.0f,
      .MaxAnisotropy = 1,
      .ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER,
      .BorderColor = {0.0f, 0.0f, 0.0f, 0.0f},
      .MinLOD = 0.0f,
      .MaxLOD = D3D12_FLOAT32_MAX,
  };
}

Descriptor Create(const D3d12Context& context, const D3D12_SAMPLER_DESC& desc) {
  const Descriptor descriptor = context.descriptors().AllocateSampler();
  context.device()->CreateSampler(&desc, descriptor.cpu);
  return descriptor;
}

}  // namespace

Samplers::Samplers(const D3d12Context& context, float max_anisotropy) {
  D3D12_SAMPLER_DESC material = SamplerDesc(D3D12_FILTER_MIN_MAG_MIP_LINEAR,
                                            D3D12_TEXTURE_ADDRESS_MODE_WRAP);
  if (max_anisotropy > 1.0f) {
    material.Filter = D3D12_FILTER_ANISOTROPIC;
    material.MaxAnisotropy = static_cast<UINT>(
        std::clamp(std::lround(max_anisotropy), 1L,
                   static_cast<long>(D3D12_MAX_MAXANISOTROPY)));
  }
  material_ = Create(context, material);
  linear_clamp_ =
      Create(context, SamplerDesc(D3D12_FILTER_MIN_MAG_MIP_LINEAR,
                                  D3D12_TEXTURE_ADDRESS_MODE_CLAMP));
  nearest_clamp_ =
      Create(context, SamplerDesc(D3D12_FILTER_MIN_MAG_MIP_POINT,
                                  D3D12_TEXTURE_ADDRESS_MODE_CLAMP));
  D3D12_SAMPLER_DESC shadow =
      SamplerDesc(D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT,
                  D3D12_TEXTURE_ADDRESS_MODE_BORDER);
  shadow.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
  shadow.BorderColor[0] = 1.0f;
  shadow.BorderColor[1] = 1.0f;
  shadow.BorderColor[2] = 1.0f;
  shadow.BorderColor[3] = 1.0f;
  shadow_compare_ = Create(context, shadow);
}

}  // namespace blue::render::d3d12
