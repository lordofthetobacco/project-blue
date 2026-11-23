#version 450

// One direction of the separable, depth-aware SSAO blur (render::SsaoPass).
// Input and output are half resolution: r = visibility, g = linear depth.
// Taps across a depth discontinuity get no weight, so occlusion does not
// bleed from the weapon onto the background or between objects.

layout(set = 0, binding = 0) uniform sampler2D aoImage;

// Must match BlurConstants in src/render/vulkan/passes/ssao_pass.cc.
layout(push_constant) uniform BlurConstants {
    ivec2 axis;  // (1, 0) or (0, 1)
} params;

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outResult;

const int kRadius = 4;
// Gaussian, sigma ~2.5 texels, for offsets 0..4.
const float kWeights[kRadius + 1] = float[](0.199, 0.176, 0.121, 0.065, 0.027);
// Relative depth difference at which a tap's weight has fallen to 1/e.
const float kDepthTolerance = 0.05;

void main()
{
    const ivec2 size = textureSize(aoImage, 0);
    const ivec2 center = ivec2(gl_FragCoord.xy);
    const vec2 centerSample = texelFetch(aoImage, center, 0).rg;
    const float centerDepth = centerSample.g;

    const float centerVisibility =
        centerSample.r >= 0.0 && centerSample.r <= 1.0 ? centerSample.r : 1.0;
    float sum = centerVisibility * kWeights[0];
    float weightSum = kWeights[0];
    for (int i = 1; i <= kRadius; ++i) {
        for (int side = -1; side <= 1; side += 2) {
            const ivec2 texel = clamp(center + params.axis * (i * side), ivec2(0), size - 1);
            const vec2 tap = texelFetch(aoImage, texel, 0).rg;
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
    outResult = vec4(sum / weightSum, centerDepth, 0.0, 1.0);
}
