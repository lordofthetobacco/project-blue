#include "render/passes/ssao_pass.h"

#include <array>
#include <cstdint>

#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include "core/extent.h"
#include "core/path.h"
#include "render/encoders.h"

namespace blue::render {

namespace {

// Must match SsaoConstants in shaders/ssao.frag.metal.
struct SsaoConstants {
  glm::vec4 projection;  // x: proj[0][0], y: proj[1][1], z: near plane
  glm::vec4 params;      // x: radius, y: intensity, z: max radius (pixels)
  glm::vec4 full_size;   // xy: full-resolution size, zw: reciprocal
};
static_assert(sizeof(SsaoConstants) == 48);

// Must match BlurConstants in shaders/ssao_blur.frag.metal (an int2).
struct BlurConstants {
  int32_t axis_x;
  int32_t axis_y;
};
static_assert(sizeof(BlurConstants) == 8);

}  // namespace

SsaoPass::SsaoPass(const MetalContext& context, ShaderPrograms& programs,
                   const std::string& shader_dir)
    : occlusion_(
          context, programs, shader_dir,
          {.fragment_path = core::JoinPath(shader_dir, "ssao.frag.metal"),
           .pass_constant_size = sizeof(SsaoConstants),
           .output_format = RenderTargets::kSsaoRawFormat,
           .label = "ssao"}),
      blur_horizontal_(
          context, programs, shader_dir,
          {.fragment_path = core::JoinPath(shader_dir, "ssao_blur.frag.metal"),
           .pass_constant_size = sizeof(BlurConstants),
           .output_format = RenderTargets::kSsaoRawFormat,
           .label = "ssao blur horizontal"}),
      blur_vertical_(
          context, programs, shader_dir,
          {.fragment_path = core::JoinPath(shader_dir, "ssao_blur.frag.metal"),
           .pass_constant_size = sizeof(BlurConstants),
           .output_format = RenderTargets::kSsaoFormat,
           .label = "ssao blur vertical"}) {}

void SsaoPass::BindTargets(const RenderTargets& targets,
                           const Samplers& samplers) {
  // The shaders read exact texels; the sampler is only what a full-screen
  // input set needs.
  const std::array<id<MTLTexture>, 1> depth{targets.depth().texture()};
  occlusion_.SetInputs(0, depth, samplers.nearest_clamp());
  const std::array<id<MTLTexture>, 1> raw{targets.ssao_raw().texture()};
  blur_horizontal_.SetInputs(0, raw, samplers.nearest_clamp());
  const std::array<id<MTLTexture>, 1> blurred{targets.ssao_blur().texture()};
  blur_vertical_.SetInputs(0, blurred, samplers.nearest_clamp());
  target_white_ = true;  // RenderTargets::Resize clears it.
}

void SsaoPass::Record(const FrameContext& ctx) {
  if (ctx.view.post.ssao_enabled) {
    RecordOcclusion(ctx);
    target_white_ = false;
  } else if (!target_white_) {
    RecordClearToWhite(ctx);
    target_white_ = true;
  }
}

void SsaoPass::RecordOcclusion(const FrameContext& ctx) const {
  const RenderTargets& targets = ctx.targets;
  const core::Extent2D full = targets.extent();
  const core::Extent2D half = targets.half_extent();
  const glm::mat4& projection = ctx.view.camera.projection;
  const auto full_width = static_cast<float>(full.width);
  const auto full_height = static_cast<float>(full.height);

  const SsaoConstants constants{
      .projection =
          glm::vec4(projection[0][0], projection[1][1], projection[3][2], 0.0f),
      .params =
          glm::vec4(ctx.view.post.ssao_radius, ctx.view.post.ssao_intensity,
                    kMaxRadiusScreenFraction * full_height, 0.0f),
      .full_size = glm::vec4(full_width, full_height, 1.0f / full_width,
                             1.0f / full_height),
  };
  occlusion_.Record(ctx.rec,
                    {.texture = targets.ssao_raw().texture(), .extent = half},
                    0, ConstantBytes(constants));

  const BlurConstants horizontal{.axis_x = 1, .axis_y = 0};
  blur_horizontal_.Record(
      ctx.rec, {.texture = targets.ssao_blur().texture(), .extent = half}, 0,
      ConstantBytes(horizontal));

  const BlurConstants vertical{.axis_x = 0, .axis_y = 1};
  blur_vertical_.Record(ctx.rec,
                        {.texture = targets.ssao().texture(), .extent = half},
                        0, ConstantBytes(vertical));
}

void SsaoPass::RecordClearToWhite(const FrameContext& ctx) {
  // A render pass that only clears: no draws.
  MTL4RenderPassDescriptor* descriptor =
      [[MTL4RenderPassDescriptor alloc] init];
  MTLRenderPassColorAttachmentDescriptor* color =
      descriptor.colorAttachments[0];
  color.texture = ctx.targets.ssao().texture();
  color.loadAction = MTLLoadActionClear;
  color.storeAction = MTLStoreActionStore;
  color.clearColor = MTLClearColorMake(1.0, 1.0, 1.0, 1.0);
  [BeginRenderPass(ctx.rec, descriptor, "ssao clear") endEncoding];
}

}  // namespace blue::render
