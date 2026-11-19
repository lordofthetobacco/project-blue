#include <metal_stdlib>

#include "bindings.h"

using namespace metal;

// Writes the LDR image (sRGB-encoded colour, luma in alpha) to the
// drawable, optionally through FXAA. An _sRGB drawable encodes on write,
// so the result is decoded first; the round trip is exact for 8-bit values.
//
// The FXAA path follows the structure of Timothy Lottes' FXAA 3.11 "quality"
// algorithm as published (NVIDIA white paper, 2011): local contrast test,
// edge orientation from second differences, a walk along the edge in both
// directions until the luma leaves the edge's average, a blend offset from
// the position along that span, and a separate sub-pixel term for
// single-pixel features. Implemented here from that description.

// Must match PresentConstants in src/render/passes/present_pass.cc
// (int32_t, int32_t, float, float: 16 bytes).
struct PresentConstants {
    int decodeSrgb;     // 1: the target encodes, so output linear values
    int fxaa;           // 1: anti-alias
    float2 texelSize;   // 1 / image size
};

// Must match FullscreenVaryings in fullscreen.vert.metal.
struct FullscreenVaryings {
    float4 position [[position]];  // Window coordinates (gl_FragCoord).
    float2 uv [[user(loc0)]];
};

// The pass's inputs, gathered so that the helpers take one argument.
struct PresentInputs {
    texture2d<float> ldrImage;  // bilinear, clamped
    sampler ldrSampler;
    float2 texelSize;
};

// Contrast below max(kEdgeThresholdMin, kEdgeThreshold * local max luma)
// is left alone (the "quality" preset's defaults).
constant float kEdgeThreshold = 0.125;
constant float kEdgeThresholdMin = 0.0312;
// How much single-pixel features are softened, [0, 1].
constant float kSubpixelQuality = 0.75;
// Step sizes of the walk along an edge, in pixels: fine near the start,
// then growing so long edges end within a fixed number of taps.
constant int kSearchSteps = 12;
constant float kSearchStepSizes[kSearchSteps] =
    {1.0, 1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0};

static float4 SampleLdr(thread const PresentInputs& inputs, float2 uv)
{
    return inputs.ldrImage.sample(inputs.ldrSampler, uv, level(0.0));
}

static float Luma(thread const PresentInputs& inputs, float2 uv)
{
    return SampleLdr(inputs, uv).a;
}

static float LumaAt(thread const PresentInputs& inputs, float2 uv, float2 offsetPixels)
{
    return SampleLdr(inputs, uv + offsetPixels * inputs.texelSize).a;
}

static float3 Fxaa(thread const PresentInputs& inputs, float2 uv)
{
    const float4 centerSample = SampleLdr(inputs, uv);
    const float center = centerSample.a;
    const float north = LumaAt(inputs, uv, float2(0.0, -1.0));
    const float south = LumaAt(inputs, uv, float2(0.0, 1.0));
    const float west = LumaAt(inputs, uv, float2(-1.0, 0.0));
    const float east = LumaAt(inputs, uv, float2(1.0, 0.0));

    const float lumaMax = max(center, max(max(north, south), max(west, east)));
    const float lumaMin = min(center, min(min(north, south), min(west, east)));
    const float contrast = lumaMax - lumaMin;
    if (contrast < max(kEdgeThresholdMin, lumaMax * kEdgeThreshold)) {
        return centerSample.rgb;
    }

    const float northWest = LumaAt(inputs, uv, float2(-1.0, -1.0));
    const float northEast = LumaAt(inputs, uv, float2(1.0, -1.0));
    const float southWest = LumaAt(inputs, uv, float2(-1.0, 1.0));
    const float southEast = LumaAt(inputs, uv, float2(1.0, 1.0));

    // A horizontal edge changes the most from row to row.
    const float horizontal = abs(northWest + southWest - 2.0 * west)
                           + abs(north + south - 2.0 * center) * 2.0
                           + abs(northEast + southEast - 2.0 * east);
    const float vertical = abs(northWest + northEast - 2.0 * north)
                         + abs(west + east - 2.0 * center) * 2.0
                         + abs(southWest + southEast - 2.0 * south);
    const bool isHorizontal = horizontal >= vertical;

    // The neighbour across the edge with the larger luma step is on the
    // other side of it.
    const float negative = isHorizontal ? north : west;
    const float positive = isHorizontal ? south : east;
    const float gradientNegative = abs(negative - center);
    const float gradientPositive = abs(positive - center);
    const bool negativeSide = gradientNegative >= gradientPositive;
    const float gradientScaled = 0.25 * max(gradientNegative, gradientPositive);
    float stepAcross = isHorizontal ? inputs.texelSize.y : inputs.texelSize.x;
    float edgeLuma;
    if (negativeSide) {
        stepAcross = -stepAcross;
        edgeLuma = 0.5 * (negative + center);
    } else {
        edgeLuma = 0.5 * (positive + center);
    }

    // Walk along the edge, half a pixel over onto it, both ways.
    float2 edgeUv = uv;
    if (isHorizontal) {
        edgeUv.y += 0.5 * stepAcross;
    } else {
        edgeUv.x += 0.5 * stepAcross;
    }
    const float2 along = isHorizontal ? float2(inputs.texelSize.x, 0.0) : float2(0.0, inputs.texelSize.y);
    float2 uvBackward = edgeUv;
    float2 uvForward = edgeUv;
    float endBackward = 0.0;
    float endForward = 0.0;
    bool doneBackward = false;
    bool doneForward = false;
    for (int i = 0; i < kSearchSteps && !(doneBackward && doneForward); ++i) {
        if (!doneBackward) {
            uvBackward -= along * kSearchStepSizes[i];
            endBackward = Luma(inputs, uvBackward) - edgeLuma;
            doneBackward = abs(endBackward) >= gradientScaled;
        }
        if (!doneForward) {
            uvForward += along * kSearchStepSizes[i];
            endForward = Luma(inputs, uvForward) - edgeLuma;
            doneForward = abs(endForward) >= gradientScaled;
        }
    }

    const float distanceBackward = isHorizontal ? uv.x - uvBackward.x : uv.y - uvBackward.y;
    const float distanceForward = isHorizontal ? uvForward.x - uv.x : uvForward.y - uv.y;
    const bool backwardCloser = distanceBackward < distanceForward;
    const float distanceToEnd = min(distanceBackward, distanceForward);
    const float spanLength = distanceBackward + distanceForward;

    // Blend only when the nearer edge end goes the right way: the luma there
    // must cross the edge average in the direction the centre lies.
    const bool centerBelowEdge = center < edgeLuma;
    const float endLuma = backwardCloser ? endBackward : endForward;
    const bool consistent = (endLuma < 0.0) != centerBelowEdge;
    const float edgeOffset = consistent ? 0.5 - distanceToEnd / spanLength : 0.0;

    // Sub-pixel aliasing: how much the centre differs from its 3x3 average.
    const float average = (2.0 * (north + south + west + east)
                         + northWest + northEast + southWest + southEast) / 12.0;
    const float subpixel = clamp(abs(average - center) / contrast, 0.0, 1.0);
    const float subpixelSmooth = (-2.0 * subpixel + 3.0) * subpixel * subpixel;
    const float subpixelOffset = subpixelSmooth * subpixelSmooth * kSubpixelQuality;

    const float offset = max(edgeOffset, subpixelOffset);
    float2 finalUv = uv;
    if (isHorizontal) {
        finalUv.y += offset * stepAcross;
    } else {
        finalUv.x += offset * stepAcross;
    }
    return SampleLdr(inputs, finalUv).rgb;
}

static float3 SrgbToLinear(float3 encoded)
{
    const float3 low = encoded / 12.92;
    const float3 high = pow((encoded + 0.055) / 1.055, float3(2.4));
    return select(high, low, encoded <= float3(0.04045));
}

fragment float4 FragmentMain(FullscreenVaryings varyings [[stage_in]],
                             texture2d<float> ldrImage [[texture(kTextureInput0)]],
                             sampler ldrSampler [[sampler(kSamplerInput)]],
                             constant PresentConstants& pass [[buffer(kBufferPass)]])
{
    const PresentInputs inputs = {ldrImage, ldrSampler, pass.texelSize};
    float3 encoded;
    if (pass.fxaa != 0) {
        encoded = Fxaa(inputs, varyings.position.xy * pass.texelSize);
    } else {
        encoded = ldrImage.read(uint2(varyings.position.xy)).rgb;
    }
    return float4(pass.decodeSrgb != 0 ? SrgbToLinear(encoded) : encoded, 1.0);
}
