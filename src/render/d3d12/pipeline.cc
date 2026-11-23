#include "render/d3d12/pipeline.h"

#include <d3d12.h>
#include <wrl/client.h>

#include <climits>
#include <cstdint>

#include "render/d3d12/d3d12_check.h"

namespace blue::render::d3d12 {

namespace {

// One subobject of a pipeline state stream: its type, then its value, at
// pointer alignment (the layout d3dx12's stream subobjects have). The
// padding is the point, so MSVC's note about it (C4324) is silenced here as
// d3dx12 does.
#pragma warning(push)
#pragma warning(disable : 4324)
template <D3D12_PIPELINE_STATE_SUBOBJECT_TYPE kType, typename T>
struct alignas(void*) Subobject {
  D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = kType;
  T value{};
};
#pragma warning(pop)

// A graphics pipeline as a stream, for what the classic description cannot
// express (the line rasterization mode of D3D12_RASTERIZER_DESC2).
struct GraphicsStream {
  Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE,
            ID3D12RootSignature*>
      root_signature;
  Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, D3D12_SHADER_BYTECODE> vs;
  Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE> ps;
  Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, D3D12_BLEND_DESC> blend;
  Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, UINT> sample_mask;
  Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER2,
            D3D12_RASTERIZER_DESC2>
      rasterizer;
  Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL,
            D3D12_DEPTH_STENCIL_DESC>
      depth_stencil;
  Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT,
            D3D12_INPUT_LAYOUT_DESC>
      input_layout;
  Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY,
            D3D12_PRIMITIVE_TOPOLOGY_TYPE>
      topology;
  Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS,
            D3D12_RT_FORMAT_ARRAY>
      rtv_formats;
  Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT,
            DXGI_FORMAT>
      dsv_format;
  Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, DXGI_SAMPLE_DESC>
      sample_desc;
};

// Creates `pso` through a stream, with lines rasterized as narrow
// quadrilaterals.
Microsoft::WRL::ComPtr<ID3D12PipelineState> CreateNarrowLinePipeline(
    ID3D12Device* device, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& pso) {
  const D3D12_RASTERIZER_DESC& classic = pso.RasterizerState;
  GraphicsStream stream;
  stream.root_signature.value = pso.pRootSignature;
  stream.vs.value = pso.VS;
  stream.ps.value = pso.PS;
  stream.blend.value = pso.BlendState;
  stream.sample_mask.value = pso.SampleMask;
  stream.rasterizer.value = {
      .FillMode = classic.FillMode,
      .CullMode = classic.CullMode,
      .FrontCounterClockwise = classic.FrontCounterClockwise,
      .DepthBias = static_cast<float>(classic.DepthBias),
      .DepthBiasClamp = classic.DepthBiasClamp,
      .SlopeScaledDepthBias = classic.SlopeScaledDepthBias,
      .DepthClipEnable = classic.DepthClipEnable,
      .LineRasterizationMode =
          D3D12_LINE_RASTERIZATION_MODE_QUADRILATERAL_NARROW,
      .ForcedSampleCount = classic.ForcedSampleCount,
      .ConservativeRaster = classic.ConservativeRaster,
  };
  stream.depth_stencil.value = pso.DepthStencilState;
  stream.input_layout.value = pso.InputLayout;
  stream.topology.value = pso.PrimitiveTopologyType;
  stream.rtv_formats.value.NumRenderTargets = pso.NumRenderTargets;
  for (uint32_t i = 0; i < pso.NumRenderTargets; ++i) {
    stream.rtv_formats.value.RTFormats[i] = pso.RTVFormats[i];
  }
  stream.dsv_format.value = pso.DSVFormat;
  stream.sample_desc.value = pso.SampleDesc;

  Microsoft::WRL::ComPtr<ID3D12Device2> device2;
  CheckHr(device->QueryInterface(IID_PPV_ARGS(&device2)));
  const D3D12_PIPELINE_STATE_STREAM_DESC stream_desc{sizeof(stream), &stream};
  Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
  CheckHr(device2->CreatePipelineState(&stream_desc, IID_PPV_ARGS(&pipeline)));
  return pipeline;
}

}  // namespace

D3D12_RENDER_TARGET_BLEND_DESC BlendDesc(BlendMode mode) {
  D3D12_RENDER_TARGET_BLEND_DESC desc{
      .BlendEnable = mode == BlendMode::kReplace ? FALSE : TRUE,
      .LogicOpEnable = FALSE,
      .SrcBlend = D3D12_BLEND_ONE,
      .DestBlend = D3D12_BLEND_ONE,
      .BlendOp = D3D12_BLEND_OP_ADD,
      .SrcBlendAlpha = D3D12_BLEND_ZERO,
      .DestBlendAlpha = D3D12_BLEND_ONE,
      .BlendOpAlpha = D3D12_BLEND_OP_ADD,
      .LogicOp = D3D12_LOGIC_OP_NOOP,
      .RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL,
  };
  if (mode == BlendMode::kAlpha) {
    desc.SrcBlend = D3D12_BLEND_SRC_ALPHA;
    desc.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
  }
  return desc;
}

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
  pso.InputLayout = {
      desc.input_layout.empty() ? nullptr : desc.input_layout.data(),
      static_cast<UINT>(desc.input_layout.size())};
  pso.PrimitiveTopologyType = desc.topology;
  pso.NumRenderTargets = desc.rtv_count;
  for (uint32_t i = 0; i < desc.rtv_count; ++i) {
    pso.RTVFormats[i] = desc.rtv_formats[i];
  }
  pso.DSVFormat = desc.dsv_format;
  pso.SampleDesc = {1, 0};

  Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
  if (desc.narrow_lines) {
    pipeline = CreateNarrowLinePipeline(device, pso);
  } else {
    CheckHr(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pipeline)));
  }
  pipeline->SetName(desc.name);
  return pipeline;
}

}  // namespace blue::render::d3d12
