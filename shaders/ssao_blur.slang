// One direction of the separable, depth-aware SSAO blur (render SsaoPass).
// Input and output are half resolution: r = visibility, g = linear depth.
// Taps across a depth discontinuity get no weight, so occlusion does not
// bleed from the weapon onto the background or between objects.
// Program: ssao_blur.

#include "fullscreen.slang"

BLUE_TEXTURE(Texture2D<float4>, aoImage, 0, BLUE_INPUT_SET);

// Must match BlurConstants in src/render/vulkan/passes/ssao_pass.cc.
struct BlurConstants {
    int2 axis;  // (1, 0) or (0, 1)
};
BLUE_PUSH_CONSTANTS(BlurConstants, params);

static const int kRadius = 4;
// Gaussian, sigma ~2.5 texels, for offsets 0..4.
static const float kWeights[kRadius + 1] = {0.199, 0.176, 0.121, 0.065, 0.027};
// Relative depth difference at which a tap's weight has fallen to 1/e.
static const float kDepthTolerance = 0.05;

[shader("fragment")]
float4 FragmentMain(FullscreenVaryings input) : SV_Target
{
    uint width;
    uint height;
    aoImage.GetDimensions(width, height);
    const int2 size = int2(int(width), int(height));
    const int2 center = int2(input.position.xy);
    const float2 centerSample = aoImage.Load(int3(center, 0)).rg;
    const float centerDepth = centerSample.g;

    const float centerVisibility =
        centerSample.r >= 0.0 && centerSample.r <= 1.0 ? centerSample.r : 1.0;
    float sum = centerVisibility * kWeights[0];
    float weightSum = kWeights[0];
    for (int i = 1; i <= kRadius; ++i) {
        for (int side = -1; side <= 1; side += 2) {
            const int2 texel = clamp(center + params.axis * (i * side), int2(0), size - 1);
            const float2 tap = aoImage.Load(int3(texel, 0)).rg;
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
