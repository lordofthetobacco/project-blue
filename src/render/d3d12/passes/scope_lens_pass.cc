#include "render/d3d12/passes/scope_lens_pass.h"

#include <d3d12.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include "core/log.h"
#include "render/binding_layout.h"
#include "render/d3d12/d3d12_context.h"
#include "render/d3d12/descriptor_heaps.h"
#include "render/d3d12/format.h"
#include "render/d3d12/image.h"
#include "render/d3d12/pipeline.h"
#include "render/d3d12/render_targets.h"
#include "render/d3d12/root_layout.h"
#include "render/d3d12/samplers.h"
#include "render/d3d12/upload_ring.h"
#include "render/scene_view.h"
#include "render/shader_manifest.h"

namespace blue::render::d3d12 {

namespace {

// Must match ScopeLensUniforms in shaders/scope_lens.slang (the Vulkan
// backend's ScopeLensUniforms).
struct ScopeLensUniforms {
  glm::mat4 lens;
  glm::vec4 right;          // w: magnification
  glm::vec4 up;             // w: apparent half field
  glm::vec4 axis;           // w: picture half field
  glm::vec4 lens_params;    // distortion, aberration, vignetting, uv scale
  glm::vec4 eye;            // lateral xy, relief error, pupil shadow depth
  glm::vec4 pupils;         // exit, eye, picture rendered, min line pixels
  glm::vec4 reticle_color;  // w: line count
  glm::vec4 glass_tint;
  std::array<glm::vec4, kMaxScopeReticleLines> reticle_lines;
  std::array<glm::vec4, kMaxScopeReticleLines / 4> reticle_half_widths;
};
static_assert(kMaxScopeReticleLines % 4 == 0);
static_assert(offsetof(ScopeLensUniforms, glass_tint) == 176);
static_assert(offsetof(ScopeLensUniforms, reticle_lines) == 192);
static_assert(offsetof(ScopeLensUniforms, reticle_half_widths) ==
              192 + 16 * kMaxScopeReticleLines);

// A constant buffer view covers a multiple of 256 bytes.
constexpr uint64_t kUniformsViewBytes = AlignUp(
    sizeof(ScopeLensUniforms), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

// The glass is generated in the vertex shader from two triangles.
constexpr UINT kVertexCount = 6;

ScopeLensUniforms MakeUniforms(const ScopeSight& scope) {
  ScopeLensUniforms uniforms{
      .lens = scope.lens,
      .right = glm::vec4(scope.right, scope.magnification),
      .up = glm::vec4(scope.up, scope.apparent_field),
      .axis = glm::vec4(scope.axis, scope.camera_tangent),
      .lens_params = glm::vec4(
          scope.distortion, scope.chromatic_aberration, scope.vignetting,
          static_cast<float>(std::min(scope.resolution, kMaxScopeResolution)) /
              static_cast<float>(kMaxScopeResolution)),
      .eye = glm::vec4(scope.eye_lateral, scope.eye_relief_error,
                       scope.pupil_shadow_depth),
      .pupils =
          glm::vec4(scope.exit_pupil_radius, scope.eye_pupil_radius,
                    scope.render_picture ? 1.0f : 0.0f, scope.min_line_pixels),
      .reticle_color =
          glm::vec4(scope.reticle_color,
                    static_cast<float>(std::min<uint32_t>(
                        scope.reticle_line_count,
                        static_cast<uint32_t>(kMaxScopeReticleLines)))),
      .glass_tint = glm::vec4(scope.glass_tint, 0.0f),
      .reticle_lines = scope.reticle_lines,
      .reticle_half_widths = {},
  };
  for (size_t i = 0; i < kMaxScopeReticleLines; ++i) {
    uniforms.reticle_half_widths[i / 4][static_cast<glm::length_t>(i % 4)] =
        scope.reticle_half_widths[i];
  }
  return uniforms;
}

// Returns the index of register `shader_register` in table `param`.
uint32_t TableOffset(const RootParam& param, uint32_t shader_register) {
  for (uint32_t i = 0; i < param.range_count; ++i) {
    const RootRange& range = param.ranges[i];
    if (shader_register >= range.first_register &&
        shader_register < range.first_register + range.count) {
      return range.table_offset + (shader_register - range.first_register);
    }
  }
  core::log::Fatal("Scope table without register {}", shader_register);
}

}  // namespace

ScopeLensPass::ScopeLensPass(const D3d12Context& context,
                             ShaderPrograms& programs,
                             const RootSignature& root_signature,
                             const FrameGlobals& frame_globals,
                             const Samplers& samplers, UploadRing& upload_ring)
    : context_(context),
      root_signature_(root_signature),
      params_(SceneRootParams::Of(root_signature.layout(), kScopeBindings)),
      frame_globals_(frame_globals),
      samplers_(samplers),
      upload_ring_(upload_ring) {
  const RootParam& table = root_signature.layout().params[params_.pass_table];
  uniforms_offset_ = TableOffset(table, BLUE_SCOPE_UNIFORMS_BINDING);
  picture_offset_ = TableOffset(table, BLUE_SCOPE_PICTURE_BINDING);
  table_size_ = table.table_size();

  programs.Add(ShaderProgram::kScopeLens,
               [this](std::span<const std::byte> vertex,
                      std::span<const std::byte> pixel) {
                 // Blended by the rim's coverage, depth tested against the
                 // scene (equal passes too, for a disc on the model's own
                 // glass) without writing depth, visible from both sides.
                 GraphicsPipelineDesc desc;
                 desc.root_signature = root_signature_.get();
                 desc.vertex = vertex;
                 desc.pixel = pixel;
                 desc.rtv_formats[0] = RenderTargets::kHdrFormat;
                 desc.rtv_count = 1;
                 desc.dsv_format = RenderTargets::kDepthFormat;
                 desc.depth_test = true;
                 desc.depth_write = false;
                 desc.depth_func = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
                 desc.blend = BlendDesc(BlendMode::kAlpha);
                 desc.name = L"blue scope lens pipeline";
                 pipeline_ = CreateGraphicsPipeline(context_.device(), desc);
               });
}

void ScopeLensPass::Draw(const FrameContext& ctx) const {
  if (!ctx.view.scope || picture_ == nullptr) {
    return;
  }
  const ScopeLensUniforms uniforms = MakeUniforms(*ctx.view.scope);
  const UploadAllocation allocation = upload_ring_.Allocate(kUniformsViewBytes);
  std::memcpy(allocation.cpu, &uniforms, sizeof(uniforms));

  DescriptorHeaps& heaps = context_.descriptors();
  const Descriptor table = heaps.AllocateTransient(table_size_);
  const D3D12_CONSTANT_BUFFER_VIEW_DESC cbv{
      .BufferLocation = allocation.gpu,
      .SizeInBytes = static_cast<UINT>(kUniformsViewBytes),
  };
  context_.device()->CreateConstantBufferView(
      &cbv, heaps.Offset(table.cpu, uniforms_offset_));
  heaps.Copy(1, heaps.Offset(table.cpu, picture_offset_),
             picture_->staging_srv());

  ID3D12GraphicsCommandList* cmd = ctx.cmd;
  cmd->SetGraphicsRootSignature(root_signature_.get());
  cmd->SetPipelineState(pipeline_.Get());
  frame_globals_.Bind(cmd, params_, ctx.frame);
  cmd->SetGraphicsRootDescriptorTable(params_.pass_table, table.gpu);
  cmd->SetGraphicsRootDescriptorTable(params_.pass_sampler,
                                      samplers_.linear_clamp());
  cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  cmd->DrawInstanced(kVertexCount, 1, 0, 0);
  ++ctx.stats.draw_calls;
}

}  // namespace blue::render::d3d12
