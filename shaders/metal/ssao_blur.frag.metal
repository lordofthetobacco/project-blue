#include <metal_stdlib>

#include "bindings.h"

using namespace metal;

// One direction of the separable, depth-aware SSAO blur (render::SsaoPass).
// Input and output are half resolution: r = visibility, g = linear depth.
// Taps across a depth discontinuity get no weight, so occlusion does not
// bleed from the weapon onto the background or between objects.

// Must match BlurConstants in src/render/passes/ssao_pass.mm.
struct BlurConstants {
    int2 axis;  // (1, 0) or (0, 1)
};

// Must match FullscreenVaryings in fullscreen.vert.metal.
struct FullscreenVaryings {
    float4 position [[position]];
    float2 uv [[user(loc0)]];
};

constant int kRadius = 4;
// Gaussian, sigma ~2.5 texels, for offsets 0..4.
constant float kWeights[kRadius + 1] = {0.199, 0.176, 0.121, 0.065, 0.027};
// Relative depth difference at which a tap's weight has fallen to 1/e.
constant float kDepthTolerance = 0.05;

fragment float4 FragmentMain(FullscreenVaryings varyings [[stage_in]],
                             texture2d<float> aoImage [[texture(kTextureInput0)]],
                             constant BlurConstants& params [[buffer(kBufferPass)]])
{
    const int2 size = int2(aoImage.get_width(), aoImage.get_height());
    const int2 center = int2(varyings.position.xy);
    const float2 centerSample = aoImage.read(uint2(center)).rg;
    const float centerDepth = centerSample.g;

    const float centerVisibility =
        centerSample.r >= 0.0 && centerSample.r <= 1.0 ? centerSample.r : 1.0;
    float sum = centerVisibility * kWeights[0];
    float weightSum = kWeights[0];
    for (int i = 1; i <= kRadius; ++i) {
        for (int side = -1; side <= 1; side += 2) {
            const int2 texel = clamp(center + params.axis * (i * side), int2(0), size - 1);
            const float2 tap = aoImage.read(uint2(texel)).rg;
            if (!(tap.r >= 0.0 && tap.r <= 1.0)) {
                continue;  // Never spread a broken (NaN) value.
            }
            const float depthWeight =
                exp(-abs(tap.g - centerDepth) / (kDepthTolerance * centerDepth + 1.0e-3));
            const float weight = kWeights[i] * depthWeight;
            sum += tap.r * weight;
            weightSum += weight;
        }
    }
    return float4(sum / weightSum, centerDepth, 0.0, 1.0);
}
