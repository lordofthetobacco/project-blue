#include "render/passes/scope_lens_pass.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <utility>

#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include "core/path.h"
#include "render/render_targets.h"
#include "render/scene_view.h"
#include "shaders/bindings.h"

namespace blue::render {

namespace {

// Must match ScopeLensUniforms in shaders/scope_lens.h.
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
static_assert(offsetof(ScopeLensUniforms, right) == 64);
static_assert(offsetof(ScopeLensUniforms, up) == 80);
static_assert(offsetof(ScopeLensUniforms, axis) == 96);
static_assert(offsetof(ScopeLensUniforms, lens_params) == 112);
static_assert(offsetof(ScopeLensUniforms, eye) == 128);
static_assert(offsetof(ScopeLensUniforms, pupils) == 144);
static_assert(offsetof(ScopeLensUniforms, reticle_color) == 160);
static_assert(offsetof(ScopeLensUniforms, glass_tint) == 176);
static_assert(offsetof(ScopeLensUniforms, reticle_lines) == 192);
static_assert(offsetof(ScopeLensUniforms, reticle_half_widths) ==
              192 + 16 * kMaxScopeReticleLines);
static_assert(sizeof(ScopeLensUniforms) == 352);

// The glass is generated in the vertex shader from two triangles.
constexpr NSUInteger kVertexCount = 6;

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

}  // namespace

ScopeLensPass::ScopeLensPass(const MetalContext& context,
                             ShaderPrograms& programs,
                             const std::string& shader_dir) {
  for (Buffer& buffer : uniforms_) {
    buffer = Buffer(context, sizeof(ScopeLensUniforms),
                    Buffer::Access::kCpuSequentialWrite, "scope lens uniforms");
  }

  id<MTLDevice> device = context.device();
  programs.Add(
      core::JoinPath(shader_dir, "scope_lens.vert.metal"),
      core::JoinPath(shader_dir, "scope_lens.frag.metal"),
      [this, device](const shader::ShaderLibrary& vertex,
                     const shader::ShaderLibrary* fragment)
          -> std::optional<std::string> {
        // Blended by the rim's coverage, depth tested against the scene
        // (equal passes too, for a disc on the model's own glass) without
        // writing depth, visible from both sides.
        const GraphicsPipelineDesc desc{
            .vertex = &vertex,
            .fragment = fragment,
            .color_format = RenderTargets::kHdrFormat,
            .depth_format = RenderTargets::kDepthFormat,
            .cull_mode = MTLCullModeNone,
            .depth_compare = MTLCompareFunctionGreaterEqual,
            .depth_write = false,
            .blend = BlendMode::kAlpha,
            .label = "scope lens",
        };
        auto pipeline = Pipeline::Create(device, desc);
        if (!pipeline.ok()) {
          return pipeline.error();
        }
        pipeline_ = std::move(pipeline).value();
        return std::nullopt;
      });
}

void ScopeLensPass::BindPicture(id<MTLTexture> picture) { picture_ = picture; }

void ScopeLensPass::Draw(const FrameContext& ctx,
                         id<MTLRenderCommandEncoder> encoder) const {
  if (!ctx.view.scope || picture_ == nil) {
    return;
  }
  const Buffer& buffer = uniforms_[ctx.frame_index];
  const ScopeLensUniforms uniforms = MakeUniforms(*ctx.view.scope);
  std::memcpy(buffer.mapped(), &uniforms, sizeof(uniforms));

  pipeline_->Bind(encoder);
  [encoder setVertexBuffer:buffer.handle() offset:0 atIndex:kBufferScopeLens];
  [encoder setFragmentBuffer:buffer.handle() offset:0 atIndex:kBufferScopeLens];
  [encoder setFragmentTexture:picture_ atIndex:kTextureScopePicture];
  [encoder drawPrimitives:MTLPrimitiveTypeTriangle
              vertexStart:0
              vertexCount:kVertexCount];
  ++ctx.stats.draw_calls;
}

}  // namespace blue::render
