#ifndef BLUE_RENDER_D3D12_FULLSCREEN_PASS_H_
#define BLUE_RENDER_D3D12_FULLSCREEN_PASS_H_

#include <d3d12.h>
#include <wrl/client.h>

#include <cstddef>
#include <cstdint>
#include <span>

#include "core/extent.h"
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
// Note for the passes of package D2: the shared vertex stage puts uv (0, 0)
// at clip (-1, -1), the top row on Vulkan (whose full-screen viewport is
// not flipped) but the bottom row on Direct3D. Programs that read
// SV_Position (present) agree on both; programs that read `uv` (bloom,
// environment filtering) see it flipped on Direct3D 12 until the vertex
// stage accounts for the API.
//
// Borrows `context` and `root_signature`. Neither copyable nor movable.
class FullscreenPass {
 public:
  FullscreenPass(const D3d12Context& context, ShaderPrograms& programs,
                 const RootSignature& root_signature, ShaderProgram program,
                 DXGI_FORMAT target_format);

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
