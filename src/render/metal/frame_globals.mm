#include "render/metal/frame_globals.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "render/metal/encoders.h"
#include "shaders/metal/bindings.h"

namespace blue::render::metal {

namespace {

// Returns a 1 x 1 image (6 layers for a cube) whose texels are `texel`.
Image MakeDefaultImage(const MetalContext& context, MTLPixelFormat format,
                       std::span<const std::byte> texel, bool cube,
                       const char* label) {
  const uint32_t layers = cube ? 6 : 1;
  std::array<std::byte, 6 * 16> texels{};
  for (uint32_t layer = 0; layer < layers; ++layer) {
    std::memcpy(texels.data() + layer * texel.size(), texel.data(),
                texel.size());
  }
  return CreateSampledImage(context,
                            {.extent = {1, 1},
                             .format = format,
                             .array_layers = layers,
                             .cube = cube,
                             .label = label},
                            std::span(texels).first(texel.size() * layers));
}

}  // namespace

FrameGlobals::FrameGlobals(const MetalContext& context,
                           const Samplers& samplers)
    : samplers_(samplers) {
  constexpr std::array<std::byte, 8> kZeroHalf4{};
  constexpr std::array<std::byte, 4> kZeroHalf2{};
  constexpr std::array<std::byte, 1> kWhiteR8{std::byte{255}};
  default_cube_ = MakeDefaultImage(context, MTLPixelFormatRGBA16Float,
                                   kZeroHalf4, /*cube=*/true,
                                   "Default environment");
  default_lut_ = MakeDefaultImage(context, MTLPixelFormatRG16Float, kZeroHalf2,
                                  /*cube=*/false, "Default BRDF LUT");
  default_ambient_occlusion_ =
      MakeDefaultImage(context, MTLPixelFormatR8Unorm, kWhiteR8,
                       /*cube=*/false, "Default ambient occlusion");

  for (Buffer& buffer : uniforms_) {
    buffer = Buffer(context, sizeof(FrameUniforms),
                    Buffer::Access::kCpuSequentialWrite, "Frame uniforms");
  }
  BindEnvironment(default_cube_.texture(), default_lut_.texture());
  BindAmbientOcclusion(default_ambient_occlusion_.texture());
}

void FrameGlobals::Write(size_t frame, const FrameUniforms& uniforms,
                         FrameView view) const {
  std::memcpy(uniforms_[Index(frame, view)].mapped(), &uniforms,
              sizeof(uniforms));
}

void FrameGlobals::BindShadowMap(id<MTLTexture> shadow_map) {
  shadow_map_ = shadow_map;
}

void FrameGlobals::BindEnvironment(id<MTLTexture> prefiltered_cube,
                                   id<MTLTexture> brdf_lut) {
  environment_ = prefiltered_cube;
  brdf_lut_ = brdf_lut;
}

void FrameGlobals::BindAmbientOcclusion(id<MTLTexture> ambient_occlusion) {
  ambient_occlusion_ = ambient_occlusion;
}

void FrameGlobals::Bind(const Recording& rec, size_t frame,
                        FrameView view) const {
  BindBuffer(rec, kBufferFrame, uniforms_[Index(frame, view)].handle());
  BindTexture(rec, kTextureShadowMap, shadow_map_);
  BindTexture(rec, kTextureEnvironment, environment_);
  BindTexture(rec, kTextureBrdfLut, brdf_lut_);
  // The scope picture has no SSAO of its own.
  BindTexture(rec, kTextureAmbientOcclusion,
              view == FrameView::kMain ? ambient_occlusion_
                                       : default_ambient_occlusion_.texture());
  BindSampler(rec, kSamplerShadow, samplers_.shadow());
  BindSampler(rec, kSamplerLinearClamp, samplers_.linear_clamp());
  BindSampler(rec, kSamplerNearestClamp, samplers_.nearest_clamp());
}

}  // namespace blue::render::metal
