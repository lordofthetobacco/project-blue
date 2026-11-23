#include "render/d3d12/fullscreen_pass.h"

#include <d3d12.h>

#include <cstddef>
#include <cstdint>
#include <span>

#include "core/extent.h"
#include "core/log.h"
#include "render/binding_layout.h"
#include "render/d3d12/d3d12_context.h"
#include "render/d3d12/descriptor_heaps.h"
#include "render/d3d12/pipeline.h"
#include "render/d3d12/root_layout.h"
#include "render/d3d12/root_signature.h"
#include "render/d3d12/shader_programs.h"
#include "render/shader_manifest.h"

namespace blue::render::d3d12 {

namespace {

// Root parameters of FullscreenRootLayout().
constexpr uint32_t kInputsParam = 1;
constexpr uint32_t kSamplerParam = 2;

}  // namespace

FullscreenPass::FullscreenPass(const D3d12Context& context,
                               ShaderPrograms& programs,
                               const RootSignature& root_signature,
                               ShaderProgram program, DXGI_FORMAT target_format,
                               bool additive)
    : context_(context), root_signature_(root_signature) {
  const RootLayout& layout = root_signature.layout();
  if (layout.Find(RootParamKind::kTable, kInputSet, 0) != kInputsParam ||
      layout.Find(RootParamKind::kSamplerTable, kInputSet,
                  kFullscreenSampler.binding) != kSamplerParam) {
    core::log::Fatal("FullscreenPass needs the full-screen root signature");
  }
  programs.Add(program, [this, target_format, additive](
                            std::span<const std::byte> vertex,
                            std::span<const std::byte> pixel) {
    GraphicsPipelineDesc desc;
    desc.root_signature = root_signature_.get();
    desc.vertex = vertex;
    desc.pixel = pixel;
    desc.rtv_formats[0] = target_format;
    desc.rtv_count = 1;
    if (additive) {
      desc.blend.BlendEnable = TRUE;
      desc.blend.SrcBlend = D3D12_BLEND_ONE;
      desc.blend.DestBlend = D3D12_BLEND_ONE;
      desc.blend.SrcBlendAlpha = D3D12_BLEND_ONE;
      desc.blend.DestBlendAlpha = D3D12_BLEND_ONE;
    }
    desc.name = L"blue fullscreen pipeline";
    pipeline_ = CreateGraphicsPipeline(context_.device(), desc);
  });
}

void FullscreenPass::Record(ID3D12GraphicsCommandList* cmd,
                            D3D12_CPU_DESCRIPTOR_HANDLE target,
                            core::Extent2D extent,
                            std::span<const D3D12_CPU_DESCRIPTOR_HANDLE> inputs,
                            D3D12_GPU_DESCRIPTOR_HANDLE sampler,
                            std::span<const std::byte> constants) const {
  const D3D12_RECT scissor{0, 0, static_cast<LONG>(extent.width),
                           static_cast<LONG>(extent.height)};
  Record(cmd, target, extent, scissor, inputs, sampler, constants);
}

void FullscreenPass::Record(ID3D12GraphicsCommandList* cmd,
                            D3D12_CPU_DESCRIPTOR_HANDLE target,
                            core::Extent2D extent, const D3D12_RECT& scissor,
                            std::span<const D3D12_CPU_DESCRIPTOR_HANDLE> inputs,
                            D3D12_GPU_DESCRIPTOR_HANDLE sampler,
                            std::span<const std::byte> constants) const {
  if (inputs.size() > kMaxFullscreenInputs ||
      constants.size() > kMaxPushConstantBytes || constants.size() % 4 != 0) {
    core::log::Fatal("Full-screen pass: {} inputs, {} bytes of constants",
                     inputs.size(), constants.size());
  }
  DescriptorHeaps& heaps = context_.descriptors();
  const Descriptor table = heaps.AllocateTransient(kMaxFullscreenInputs);
  for (uint32_t i = 0; i < kMaxFullscreenInputs; ++i) {
    heaps.Copy(1, heaps.Offset(table.cpu, i),
               i < inputs.size() ? inputs[i] : heaps.null_texture_srv());
  }

  cmd->OMSetRenderTargets(1, &target, FALSE, nullptr);
  const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(extent.width),
                                static_cast<float>(extent.height), 0.0f, 1.0f};
  cmd->RSSetViewports(1, &viewport);
  cmd->RSSetScissorRects(1, &scissor);
  cmd->SetGraphicsRootSignature(root_signature_.get());
  cmd->SetPipelineState(pipeline_.Get());
  if (!constants.empty()) {
    cmd->SetGraphicsRoot32BitConstants(
        kPushConstantsParam, static_cast<UINT>(constants.size() / 4),
        constants.data(), 0);
  }
  cmd->SetGraphicsRootDescriptorTable(kInputsParam, table.gpu);
  cmd->SetGraphicsRootDescriptorTable(kSamplerParam, sampler);
  cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  cmd->DrawInstanced(3, 1, 0, 0);
}

}  // namespace blue::render::d3d12
