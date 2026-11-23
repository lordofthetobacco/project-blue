#ifndef BLUE_RENDER_D3D12_PIPELINE_H_
#define BLUE_RENDER_D3D12_PIPELINE_H_

#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace blue::render::d3d12 {

// How render target 0 combines a pipeline's output with what it holds; the
// Vulkan backend's BlendMode, factor for factor (the target's alpha is kept
// by kAdditive and kAlpha).
enum class BlendMode : uint8_t {
  kReplace,   // No blending.
  kAdditive,  // colour: ONE + ONE.
  kAlpha,     // colour: SRC_ALPHA + INV_SRC_ALPHA.
};

// Returns the blend state of render target 0 for `mode`.
[[nodiscard]] D3D12_RENDER_TARGET_BLEND_DESC BlendDesc(BlendMode mode);

// How a graphics pipeline state object is made from a program's DXIL. The
// defaults are a full-screen pass: no vertex input, no culling, no depth,
// no blending, one render target.
struct GraphicsPipelineDesc {
  ID3D12RootSignature* root_signature = nullptr;
  // DXIL containers; `pixel` empty for depth-only programs.
  std::span<const std::byte> vertex;
  std::span<const std::byte> pixel;
  std::span<const D3D12_INPUT_ELEMENT_DESC> input_layout;
  std::array<DXGI_FORMAT, 8> rtv_formats{};
  uint32_t rtv_count = 0;
  DXGI_FORMAT dsv_format = DXGI_FORMAT_UNKNOWN;
  D3D12_CULL_MODE cull = D3D12_CULL_MODE_NONE;
  // Front faces wind counter-clockwise (the Vulkan backend's convention).
  bool front_counter_clockwise = true;
  bool depth_test = false;
  bool depth_write = false;
  D3D12_COMPARISON_FUNC depth_func = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
  bool depth_clip = true;
  int32_t depth_bias = 0;
  float slope_scaled_depth_bias = 0.0f;
  // Blend state of render target 0 (the others: no blending).
  D3D12_RENDER_TARGET_BLEND_DESC blend = BlendDesc(BlendMode::kReplace);
  D3D12_PRIMITIVE_TOPOLOGY_TYPE topology =
      D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  // Lines as one-pixel-wide rectangles (Vulkan's strict lines) instead of
  // the aliased diamond-exit rule; only where the device supports it
  // (D3d12Context::narrow_lines_supported()).
  bool narrow_lines = false;
  const wchar_t* name = L"blue pipeline";
};

// Creates the pipeline state object described by `desc`; fatal on failure.
[[nodiscard]] Microsoft::WRL::ComPtr<ID3D12PipelineState>
CreateGraphicsPipeline(ID3D12Device* device, const GraphicsPipelineDesc& desc);

}  // namespace blue::render::d3d12

#endif  // BLUE_RENDER_D3D12_PIPELINE_H_
