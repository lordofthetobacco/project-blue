#version 450

// Writes the LDR image (sRGB-encoded colour, luma in alpha) to the
// swapchain, optionally through FXAA. An _SRGB swapchain encodes on write,
// so the result is decoded first; the round trip is exact for 8-bit values.
//
// The FXAA path follows the structure of Timothy Lottes' FXAA 3.11 "quality"
// algorithm as published (NVIDIA white paper, 2011): local contrast test,
// edge orientation from second differences, a walk along the edge in both
// directions until the luma leaves the edge's average, a blend offset from
// the position along that span, and a separate sub-pixel term for
// single-pixel features. Implemented here from that description.

layout(set = 0, binding = 0) uniform sampler2D ldrImage;  // bilinear, clamped

// Must match PresentConstants in src/render/passes/present_pass.cc.
layout(push_constant) uniform PresentConstants {
    int decodeSrgb;   // 1: the target encodes, so output linear values
    int fxaa;         // 1: anti-alias
    vec2 texelSize;   // 1 / image size
} params;

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

// Contrast below max(kEdgeThresholdMin, kEdgeThreshold * local max luma)
// is left alone (the "quality" preset's defaults).
const float kEdgeThreshold = 0.125;
const float kEdgeThresholdMin = 0.0312;
// How much single-pixel features are softened, [0, 1].
const float kSubpixelQuality = 0.75;
// Step sizes of the walk along an edge, in pixels: fine near the start,
// then growing so long edges end within a fixed number of taps.
const int kSearchSteps = 12;
const float kSearchStepSizes[kSearchSteps] =
    float[](1.0, 1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0);

float Luma(vec2 uv)
{
    return textureLod(ldrImage, uv, 0.0).a;
}

float LumaAt(vec2 uv, vec2 offsetPixels)
{
    return textureLod(ldrImage, uv + offsetPixels * params.texelSize, 0.0).a;
}

vec3 Fxaa(vec2 uv)
{
    const vec4 centerSample = textureLod(ldrImage, uv, 0.0);
    const float center = centerSample.a;
    const float north = LumaAt(uv, vec2(0.0, -1.0));
    const float south = LumaAt(uv, vec2(0.0, 1.0));
    const float west = LumaAt(uv, vec2(-1.0, 0.0));
    const float east = LumaAt(uv, vec2(1.0, 0.0));

    const float lumaMax = max(center, max(max(north, south), max(west, east)));
    const float lumaMin = min(center, min(min(north, south), min(west, east)));
    const float contrast = lumaMax - lumaMin;
    if (contrast < max(kEdgeThresholdMin, lumaMax * kEdgeThreshold)) {
        return centerSample.rgb;
    }

    const float northWest = LumaAt(uv, vec2(-1.0, -1.0));
    const float northEast = LumaAt(uv, vec2(1.0, -1.0));
    const float southWest = LumaAt(uv, vec2(-1.0, 1.0));
    const float southEast = LumaAt(uv, vec2(1.0, 1.0));

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
    float stepAcross = isHorizontal ? params.texelSize.y : params.texelSize.x;
    float edgeLuma;
    if (negativeSide) {
        stepAcross = -stepAcross;
        edgeLuma = 0.5 * (negative + center);
    } else {
        edgeLuma = 0.5 * (positive + center);
    }

    // Walk along the edge, half a pixel over onto it, both ways.
    vec2 edgeUv = uv;
    if (isHorizontal) {
        edgeUv.y += 0.5 * stepAcross;
    } else {
        edgeUv.x += 0.5 * stepAcross;
    }
    const vec2 along = isHorizontal ? vec2(params.texelSize.x, 0.0) : vec2(0.0, params.texelSize.y);
    vec2 uvBackward = edgeUv;
    vec2 uvForward = edgeUv;
    float endBackward = 0.0;
    float endForward = 0.0;
    bool doneBackward = false;
    bool doneForward = false;
    for (int i = 0; i < kSearchSteps && !(doneBackward && doneForward); ++i) {
        if (!doneBackward) {
            uvBackward -= along * kSearchStepSizes[i];
            endBackward = Luma(uvBackward) - edgeLuma;
            doneBackward = abs(endBackward) >= gradientScaled;
        }
        if (!doneForward) {
            uvForward += along * kSearchStepSizes[i];
            endForward = Luma(uvForward) - edgeLuma;
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
    vec2 finalUv = uv;
    if (isHorizontal) {
        finalUv.y += offset * stepAcross;
    } else {
        finalUv.x += offset * stepAcross;
    }
    return textureLod(ldrImage, finalUv, 0.0).rgb;
}

vec3 SrgbToLinear(vec3 encoded)
{
    const vec3 low = encoded / 12.92;
    const vec3 high = pow((encoded + 0.055) / 1.055, vec3(2.4));
    return mix(high, low, lessThanEqual(encoded, vec3(0.04045)));
}

void main()
{
    vec3 encoded;
    if (params.fxaa != 0) {
        encoded = Fxaa(gl_FragCoord.xy * params.texelSize);
    } else {
        encoded = texelFetch(ldrImage, ivec2(gl_FragCoord.xy), 0).rgb;
    }
    outColor = vec4(params.decodeSrgb != 0 ? SrgbToLinear(encoded) : encoded, 1.0);
}
