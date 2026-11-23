#include "render/passes/present_pass.h"

#include <array>
#include <cstdint>

#include "core/path.h"

namespace blue::render {

namespace {

// Must match PresentConstants in shaders/present.frag.metal.
struct PresentConstants {
  int32_t decode_srgb;  // 1: the target encodes, so output linear values.
  int32_t fxaa;         // 1: anti-alias.
  float texel_width;    // 1 / image size
  float texel_height;
};
static_assert(sizeof(PresentConstants) == 16);

bool IsSrgb(MTLPixelFormat format) {
  return format == MTLPixelFormatBGRA8Unorm_sRGB ||
         format == MTLPixelFormatRGBA8Unorm_sRGB;
}

}  // namespace

PresentPass::PresentPass(const MetalContext& context, ShaderPrograms& programs,
                         const std::string& shader_dir,
                         MTLPixelFormat output_format)
    : srgb_output_(IsSrgb(output_format)),
      pass_(context, programs, shader_dir,
            {.fragment_path = core::JoinPath(shader_dir, "present.frag.metal"),
             .input_count = 1,
             .pass_constant_size = sizeof(PresentConstants),
             .output_format = output_format,
             .label = "present"}) {}

void PresentPass::BindTargets(const RenderTargets& targets,
                              const Samplers& samplers) {
  const std::array<id<MTLTexture>, 1> inputs{targets.ldr().texture()};
  // FXAA needs bilinear taps; the plain copy uses read().
  pass_.SetInputs(0, inputs, samplers.linear_clamp());
}

void PresentPass::Record(const FrameContext& ctx,
                         const PresentTarget& target) const {
  const core::Extent2D extent = ctx.targets.output_extent();
  const PresentConstants constants{
      .decode_srgb = srgb_output_ ? 1 : 0,
      // Not after the temporal scaler: it is the anti-aliasing then.
      .fxaa = ctx.view.post.fxaa_enabled && !ctx.temporal ? 1 : 0,
      .texel_width = 1.0f / static_cast<float>(extent.width),
      .texel_height = 1.0f / static_cast<float>(extent.height),
  };
  pass_.Record(ctx.rec, {.texture = target.texture, .extent = target.extent}, 0,
               ConstantBytes(constants));
}

}  // namespace blue::render
