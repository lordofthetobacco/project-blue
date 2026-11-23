#include <metal_stdlib>

#include "bindings.h"

using namespace metal;

#include "common.h"

// Coverage of the selected draw items (render::OverlayPass), drawn with
// depth.vert.metal: 0.5 where an item is hidden by the scene (pipeline
// without a depth test), 1 where it is visible (depth-tested pipeline, drawn
// second).

constant bool kVisible [[function_constant(kConstantSelectionVisible)]];
// The visible test is done here against the scene depth texture instead of
// by the pipeline (see line.frag.metal). Only read when kVisible.
constant bool kSampledDepth [[function_constant(kConstantSampledDepth)]];

// Must match DepthVaryings in depth.vert.metal (the uvs are not read).
struct DepthVaryings {
    float4 position [[position]];
};

fragment float4 FragmentMain(DepthVaryings in [[stage_in]],
                             constant FrameUniforms& frame [[buffer(kBufferFrame)]],
                             depth2d<float> sceneDepth [[texture(kTextureInput0), function_constant(kSampledDepth)]],
                             sampler depthSampler [[sampler(kSamplerInput), function_constant(kSampledDepth)]])
{
    if (kSampledDepth && kVisible) {
        // Reversed Z: hidden where the scene is nearer (greater). The scene
        // depth is jittered and may be coarser than this target, so allow
        // the item's own surface a little slack.
        const float2 uv = in.position.xy * frame.screen.zw;
        const float scene = sceneDepth.sample(depthSampler, uv);
        if (in.position.z < scene * (1.0 - 1.0e-3)) {
            discard_fragment();
        }
    }
    return float4(kVisible ? 1.0 : 0.5);
}
