#include "render/samplers.h"

#include "render/metal_check.h"

namespace blue::render {

namespace {

// Returns a descriptor with `filter` for magnification, minification and
// between mips, `address` on every axis and every mip reachable.
MTLSamplerDescriptor* SamplerDescriptor(MTLSamplerMinMagFilter filter,
                                        MTLSamplerAddressMode address,
                                        NSString* label) {
  MTLSamplerDescriptor* const descriptor = [[MTLSamplerDescriptor alloc] init];
  descriptor.minFilter = filter;
  descriptor.magFilter = filter;
  descriptor.mipFilter = filter == MTLSamplerMinMagFilterLinear
                             ? MTLSamplerMipFilterLinear
                             : MTLSamplerMipFilterNearest;
  descriptor.sAddressMode = address;
  descriptor.tAddressMode = address;
  descriptor.rAddressMode = address;
  descriptor.label = label;
  // Samplers are bound through the argument table (BindSampler()).
  descriptor.supportArgumentBuffers = YES;
  return descriptor;
}

id<MTLSamplerState> CreateSampler(id<MTLDevice> device,
                                  MTLSamplerDescriptor* descriptor) {
  const id<MTLSamplerState> sampler =
      [device newSamplerStateWithDescriptor:descriptor];
  FatalIfNil(sampler, nil, "newSamplerStateWithDescriptor");
  return sampler;
}

}  // namespace

Samplers::Samplers(const MetalContext& context) {
  const id<MTLDevice> device = context.device();

  MTLSamplerDescriptor* const material = SamplerDescriptor(
      MTLSamplerMinMagFilterLinear, MTLSamplerAddressModeRepeat, @"Material");
  material.maxAnisotropy = kMaterialAnisotropy;
  material_ = CreateSampler(device, material);

  linear_clamp_ = CreateSampler(
      device, SamplerDescriptor(MTLSamplerMinMagFilterLinear,
                                MTLSamplerAddressModeClampToEdge,
                                @"Linear clamp"));
  nearest_clamp_ = CreateSampler(
      device, SamplerDescriptor(MTLSamplerMinMagFilterNearest,
                                MTLSamplerAddressModeClampToEdge,
                                @"Nearest clamp"));

  MTLSamplerDescriptor* const shadow =
      SamplerDescriptor(MTLSamplerMinMagFilterLinear,
                        MTLSamplerAddressModeClampToBorderColor, @"Shadow");
  shadow.mipFilter = MTLSamplerMipFilterNotMipmapped;
  shadow.borderColor = MTLSamplerBorderColorOpaqueWhite;
  shadow.compareFunction = MTLCompareFunctionLessEqual;
  shadow_ = CreateSampler(device, shadow);
}

}  // namespace blue::render
