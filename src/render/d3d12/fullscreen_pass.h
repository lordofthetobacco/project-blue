#ifndef BLUE_RENDER_D3D12_FULLSCREEN_PASS_H_
#define BLUE_RENDER_D3D12_FULLSCREEN_PASS_H_

#include <d3d12.h>
#include <wrl/client.h>

#include <cstddef>
#include <cstdint>
#include <span>

#include "core/extent.h"
#include "render/d3d12/pipeline.h"
#include "render/d3d12/root_signature.h"
#include "render/d3d12/shader_programs.h"
#include "render/shader_manifest.h"

namespace blue::render::d3d12 {

class D3d12Context;

// One full-screen program (shaders/fullscreen.slang's vertex stage and a
// fragment stage) drawn as one triangle into one render target: owns the
// pipeline (hot-reloadable through ShaderPrograms) and records the draw
// with the full-screen root signature (FullscreenRootLayout(): push
// constants, kMaxFullscreenInputs input textures, the input sampler). The
// inputs are copied into a transient descriptor table each time, so render
// targets can be recreated freely between frames.
//
// The vertex stage puts uv (0, 0) at the top-left pixel on Direct3D as on
// Vulkan (its DXIL build mirrors clip y, see BLUE_DXIL in
// shaders/README.md), so programs that read `uv` match the Vulkan backend.
//
// Borrows `context` and `root_signature`. Neither copyable nor movable.
class FullscreenPass {
 public:
  // `blend` says how the result is combined with the target.
  FullscreenPass(const D3d12Context& context, ShaderPrograms& programs,
                 const RootSignature& root_signature, ShaderProgram program,
                 DXGI_FORMAT target_format,
                 BlendMode blend = BlendMode::kReplace);

  FullscreenPass(const FullscreenPass&) = delete;
  FullscreenPass& operator=(const FullscreenPass&) = delete;

  // Records the draw into `target` (in RENDER_TARGET) of `extent`, reading
  // `inputs` (staging SRVs of images in a shader-resource state; at most
  // kMaxFullscreenInputs) through `sampler`, with `constants` as the push
  // constants (at most kMaxPushConstantBytes).
  void Record(ID3D12GraphicsCommandList* cmd,
              D3D12_CPU_DESCRIPTOR_HANDLE target, core::Extent2D extent,
              std::span<const D3D12_CPU_DESCRIPTOR_HANDLE> inputs,
              D3D12_GPU_DESCRIPTOR_HANDLE sampler,
              std::span<const std::byte> constants) const;
  // Same, writing only the pixels inside `scissor`.
  void Record(ID3D12GraphicsCommandList* cmd,
              D3D12_CPU_DESCRIPTOR_HANDLE target, core::Extent2D extent,
              const D3D12_RECT& scissor,
              std::span<const D3D12_CPU_DESCRIPTOR_HANDLE> inputs,
              D3D12_GPU_DESCRIPTOR_HANDLE sampler,
              std::span<const std::byte> constants) const;

 private:
  const D3d12Context& context_;
  const RootSignature& root_signature_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_;
};

}  // namespace blue::render::d3d12

#endif  // BLUE_RENDER_D3D12_FULLSCREEN_PASS_H_
