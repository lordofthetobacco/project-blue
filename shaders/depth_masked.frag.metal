#include <metal_stdlib>

#include "bindings.h"

using namespace metal;

#include "material.h"

// Alpha test of the depth pre-pass for alpha-masked materials: the same
// cutoff as mesh.frag.metal, so the pre-pass keeps exactly the fragments the
// mesh pass shades. Writes depth only.

// Must match DepthVaryings in depth.vert.metal.
struct DepthVaryings {
    float4 position [[position]];
    float4 uv [[user(loc0)]];  // xy: uv0, zw: uv1
};

fragment void FragmentMain(DepthVaryings in [[stage_in]],
                           constant MaterialParams& material [[buffer(kBufferMaterial)]],
                           texture2d<float> baseColorMap [[texture(kTextureBaseColor)]],
                           sampler materialSampler [[sampler(kSamplerMaterial)]])
{
    const float alpha = baseColorMap.sample(materialSampler,
                                            MaterialUv(material, kSlotBaseColor, in.uv.xy, in.uv.zw)).a *
                        material.baseColor.a;
    if (alpha < material.uvAlpha.z) {
        discard_fragment();
    }
}
