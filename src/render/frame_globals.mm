#include "render/frame_globals.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "lighting/cascades.h"
#include "render/metal_check.h"
#include "shaders/bindings.h"

namespace blue::render {

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

// Returns a 1 x 1 shadow map with one layer per cascade, cleared to depth 1:
// every comparison passes, so everything is lit.
Image MakeDefaultShadowMap(const MetalContext& context) {
  Image map(context, {.extent = {1, 1},
                      .format = MTLPixelFormatDepth16Unorm,
                      .usage = MTLTextureUsageRenderTarget |
                               MTLTextureUsageShaderRead,
                      .array_layers = lighting::kCascadeCount,
                      .label = "Default shadow map"});
  context.SubmitImmediate([&map](id<MTLCommandBuffer> cmd) {
    for (uint32_t layer = 0; layer < map.array_layers(); ++layer) {
      MTLRenderPassDescriptor* pass =
          [MTLRenderPassDescriptor renderPassDescriptor];
      pass.depthAttachment.texture = map.texture();
      pass.depthAttachment.slice = layer;
      pass.depthAttachment.loadAction = MTLLoadActionClear;
      pass.depthAttachment.storeAction = MTLStoreActionStore;
      pass.depthAttachment.clearDepth = 1.0;
      id<MTLRenderCommandEncoder> encoder =
          [cmd renderCommandEncoderWithDescriptor:pass];
      FatalIfNil(encoder, nil, "render encoder: clear default shadow map");
      encoder.label = @"Clear default shadow map";
      [encoder endEncoding];
    }
  });
  return map;
}

}  // namespace

FrameGlobals::FrameGlobals(const MetalContext& context,
                           const Samplers& samplers)
    : samplers_(samplers) {
  constexpr std::array<std::byte, 8> kZeroHalf4{};
  constexpr std::array<std::byte, 4> kZeroHalf2{};
  constexpr std::array<std::byte, 1> kWhiteR8{std::byte{255}};
  default_shadow_map_ = MakeDefaultShadowMap(context);
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
  BindShadowMap(default_shadow_map_.texture());
  BindEnvironment(default_cube_.texture(), default_lut_.texture());
  BindAmbientOcclusion(default_ambient_occlusion_.texture());
}

void FrameGlobals::Write(size_t frame, const FrameUniforms& uniforms) const {
  std::memcpy(uniforms_[frame].mapped(), &uniforms, sizeof(uniforms));
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

void FrameGlobals::Bind(id<MTLRenderCommandEncoder> encoder,
                        size_t frame) const {
  const id<MTLBuffer> buffer = uniforms_[frame].handle();
  [encoder setVertexBuffer:buffer offset:0 atIndex:kBufferFrame];
  [encoder setFragmentBuffer:buffer offset:0 atIndex:kBufferFrame];
  [encoder setFragmentTexture:shadow_map_ atIndex:kTextureShadowMap];
  [encoder setFragmentTexture:environment_ atIndex:kTextureEnvironment];
  [encoder setFragmentTexture:brdf_lut_ atIndex:kTextureBrdfLut];
  [encoder setFragmentTexture:ambient_occlusion_
                      atIndex:kTextureAmbientOcclusion];
  [encoder setFragmentSamplerState:samplers_.shadow() atIndex:kSamplerShadow];
  [encoder setFragmentSamplerState:samplers_.linear_clamp()
                           atIndex:kSamplerLinearClamp];
  [encoder setFragmentSamplerState:samplers_.nearest_clamp()
                           atIndex:kSamplerNearestClamp];
}

}  // namespace blue::render
