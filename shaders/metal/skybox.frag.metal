#include <metal_stdlib>

#include "bindings.h"

using namespace metal;

#include "common.h"
#include "environment.h"
#include "sanitize.h"

// The environment drawn as the sky behind everything (render::SkyboxPass):
// the unfiltered sky cube in the view direction, rotated and scaled like the
// environment lighting.

// Must match SkyboxVaryings in skybox.vert.metal.
struct SkyboxVaryings {
    float4 position [[position]];
    float2 ndc [[user(loc0)]];
};

fragment float4 FragmentMain(SkyboxVaryings varyings [[stage_in]],
                             constant FrameUniforms& frame [[buffer(kBufferFrame)]],
                             texturecube<float> skyMap [[texture(kTextureSky)]],
                             sampler skySampler [[sampler(kSamplerLinearClamp)]])
{
    // View-space direction through this pixel (any point on the ray works;
    // depth 1 is the near plane in reversed Z), then rotated to world space.
    const float4 point = frame.inverseProjection * float4(varyings.ndc, 1.0, 1.0);
    const float3 viewDirection = point.xyz / point.w;
    const float3x3 viewRotation = float3x3(frame.view[0].xyz, frame.view[1].xyz, frame.view[2].xyz);
    const float3 world = normalize(transpose(viewRotation) * viewDirection);
    const float3 d = RotateY(world, -frame.environmentParams.y);
    return float4(SanitizeRadiance(skyMap.sample(skySampler, d, level(0.0)).rgb * frame.environmentParams.x),
                  1.0);
}
