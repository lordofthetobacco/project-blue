#include "render/d3d12/pipeline.h"

#include <d3d12.h>
#include <wrl/client.h>

#include <climits>

#include "render/d3d12/d3d12_check.h"

namespace blue::render::d3d12 {

Microsoft::WRL::ComPtr<ID3D12PipelineState> CreateGraphicsPipeline(
    ID3D12Device* device, const GraphicsPipelineDesc& desc) {
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
  pso.pRootSignature = desc.root_signature;
  pso.VS = {desc.vertex.data(), desc.vertex.size()};
  pso.PS = {desc.pixel.empty() ? nullptr : desc.pixel.data(),
            desc.pixel.size()};
  for (D3D12_RENDER_TARGET_BLEND_DESC& target : pso.BlendState.RenderTarget) {
    target = desc.blend;
    target.BlendEnable = FALSE;
  }
  pso.BlendState.RenderTarget[0] = desc.blend;
  pso.SampleMask = UINT_MAX;
  pso.RasterizerState = {
      .FillMode = D3D12_FILL_MODE_SOLID,
      .CullMode = desc.cull,
      .FrontCounterClockwise = desc.front_counter_clockwise ? TRUE : FALSE,
      .DepthBias = desc.depth_bias,
      .DepthBiasClamp = 0.0f,
      .SlopeScaledDepthBias = desc.slope_scaled_depth_bias,
      .DepthClipEnable = desc.depth_clip ? TRUE : FALSE,
      .MultisampleEnable = FALSE,
      .AntialiasedLineEnable = FALSE,
      .ForcedSampleCount = 0,
      .ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF,
  };
  pso.DepthStencilState.DepthEnable = desc.depth_test ? TRUE : FALSE;
  pso.DepthStencilState.DepthWriteMask = desc.depth_write
                                             ? D3D12_DEPTH_WRITE_MASK_ALL
                                             : D3D12_DEPTH_WRITE_MASK_ZERO;
  pso.DepthStencilState.DepthFunc = desc.depth_func;
  pso.DepthStencilState.StencilEnable = FALSE;
  pso.InputLayout = {desc.input_layout.empty() ? nullptr
                                               : desc.input_layout.data(),
                     static_cast<UINT>(desc.input_layout.size())};
  pso.PrimitiveTopologyType = desc.topology;
  pso.NumRenderTargets = desc.rtv_count;
  for (uint32_t i = 0; i < desc.rtv_count; ++i) {
    pso.RTVFormats[i] = desc.rtv_formats[i];
  }
  pso.DSVFormat = desc.dsv_format;
  pso.SampleDesc = {1, 0};

  Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
  CheckHr(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pipeline)));
  pipeline->SetName(desc.name);
  return pipeline;
}

}  // namespace blue::render::d3d12
