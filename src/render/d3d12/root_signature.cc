#include "render/d3d12/root_signature.h"

#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <string_view>

#include "core/log.h"
#include "render/binding_layout.h"
#include "render/d3d12/d3d12_check.h"
#include "render/d3d12/root_layout.h"

namespace blue::render::d3d12 {

namespace {

D3D12_SHADER_VISIBILITY Visibility(uint8_t stages) {
  if (stages == kStageVertex) {
    return D3D12_SHADER_VISIBILITY_VERTEX;
  }
  if (stages == kStageFragment) {
    return D3D12_SHADER_VISIBILITY_PIXEL;
  }
  return D3D12_SHADER_VISIBILITY_ALL;
}

D3D12_DESCRIPTOR_RANGE_TYPE RangeType(BindingKind kind) {
  switch (kind) {
    case BindingKind::kUniformBuffer:
      return D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
    case BindingKind::kTexture:
      return D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    case BindingKind::kSampler:
      return D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
  }
  return D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
}

}  // namespace

RootSignature::RootSignature(ID3D12Device* device, const RootLayout& layout,
                             const wchar_t* name)
    : layout_(layout) {
  if (layout.cost_dwords() > 64) {
    core::log::Fatal("Root signature costs {} DWORDs (at most 64)",
                     layout.cost_dwords());
  }
  // Ranges of every table, stored here so the parameters can point at them.
  std::array<std::array<D3D12_DESCRIPTOR_RANGE1, 4>, RootLayout::kMaxParams>
      ranges{};
  std::array<D3D12_ROOT_PARAMETER1, RootLayout::kMaxParams> params{};
  bool uses_input_assembler = false;
  for (size_t i = 0; i < layout.param_count; ++i) {
    const RootParam& param = layout.params[i];
    D3D12_ROOT_PARAMETER1& out = params[i];
    out.ShaderVisibility = Visibility(param.stages);
    switch (param.kind) {
      case RootParamKind::kConstants:
        out.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        out.Constants = {.ShaderRegister = param.shader_register,
                         .RegisterSpace = param.space,
                         .Num32BitValues = param.constant_count};
        break;
      case RootParamKind::kCbv:
        out.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        out.Descriptor = {
            .ShaderRegister = param.shader_register,
            .RegisterSpace = param.space,
            .Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE};
        break;
      case RootParamKind::kTable:
        for (uint32_t r = 0; r < param.range_count; ++r) {
          const RootRange& range = param.ranges[r];
          ranges[i][r] = {
              .RangeType = RangeType(range.kind),
              .NumDescriptors = range.count,
              .BaseShaderRegister = range.first_register,
              .RegisterSpace = param.space,
              .Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE,
              .OffsetInDescriptorsFromTableStart = range.table_offset,
          };
        }
        out.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        out.DescriptorTable = {.NumDescriptorRanges = param.range_count,
                               .pDescriptorRanges = ranges[i].data()};
        break;
      case RootParamKind::kSamplerTable:
        ranges[i][0] = {
            .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER,
            .NumDescriptors = 1,
            .BaseShaderRegister = param.shader_register,
            .RegisterSpace = param.space,
            .Flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE,
            .OffsetInDescriptorsFromTableStart = 0,
        };
        out.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        out.DescriptorTable = {.NumDescriptorRanges = 1,
                               .pDescriptorRanges = ranges[i].data()};
        break;
    }
    // Scene programs (the ones with a root CBV) read vertex buffers.
    uses_input_assembler =
        uses_input_assembler || param.kind == RootParamKind::kCbv;
  }

  D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
  desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
  desc.Desc_1_1 = {
      .NumParameters = static_cast<UINT>(layout.param_count),
      .pParameters = params.data(),
      .NumStaticSamplers = 0,
      .pStaticSamplers = nullptr,
      .Flags = (uses_input_assembler
                    ? D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
                    : D3D12_ROOT_SIGNATURE_FLAG_NONE) |
               D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
               D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
               D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS,
  };
  Microsoft::WRL::ComPtr<ID3DBlob> blob;
  Microsoft::WRL::ComPtr<ID3DBlob> error;
  const HRESULT result =
      D3D12SerializeVersionedRootSignature(&desc, &blob, &error);
  if (FAILED(result)) {
    const std::string_view message =
        error ? std::string_view(static_cast<const char*>(error->GetBufferPointer()),
                                 error->GetBufferSize())
              : std::string_view();
    core::log::Fatal("Root signature serialization failed: {} {}",
                     HrToString(result), message);
  }
  CheckHr(device->CreateRootSignature(0, blob->GetBufferPointer(),
                                      blob->GetBufferSize(),
                                      IID_PPV_ARGS(&signature_)));
  signature_->SetName(name);
}

}  // namespace blue::render::d3d12
