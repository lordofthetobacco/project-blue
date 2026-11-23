#include <metal_stdlib>

#include "bindings.h"

using namespace metal;

#include "common.h"

// Debug lines (render::OverlayPass): flat colour, alpha-blended.

// The pipeline has no depth attachment and the fragment is tested against
// the scene depth texture here: the overlay's target is not the size of the
// scene depth while the temporal scaler is on.
constant bool kSampledDepth [[function_constant(kConstantSampledDepth)]];

// Must match LineVaryings in line.vert.metal.
struct LineVaryings {
    float4 position [[position]];
    float4 color [[user(loc0)]];
};

fragment float4 FragmentMain(LineVaryings in [[stage_in]],
                             constant FrameUniforms& frame [[buffer(kBufferFrame)]],
                             depth2d<float> sceneDepth [[texture(kTextureInput0), function_constant(kSampledDepth)]],
                             sampler depthSampler [[sampler(kSamplerInput), function_constant(kSampledDepth)]])
{
    if (kSampledDepth) {
        // Reversed Z: hidden where the scene is nearer (greater).
        const float2 uv = in.position.xy * frame.screen.zw;
        if (in.position.z < sceneDepth.sample(depthSampler, uv)) {
            discard_fragment();
        }
    }
    return in.color;
}
