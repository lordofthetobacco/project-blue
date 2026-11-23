#version 450

#include "motion_blur.glsl"
#include "sanitize.glsl"

// Weapon motion blur reconstruction (render::MotionBlurPass), after McGuire
// et al. 2012, "A Reconstruction Filter for Plausible Motion Blur", reduced
// to one moving layer in front of a static background. Coverage replaces
// their depth comparison: a covered pixel (a moving item) is always in front
// of an uncovered one.
//
// Each pixel gathers kTapCount jittered taps along the longest blur vector
// of its 3 x 3 tiles. A covered tap reaches the pixel when the pixel lies on
// its smear (within half its blur length) and then adds its colour with the
// density of that smear; uncovered taps never spread. The weight summed over
// the reaching taps is how much of the exposure the pixel sees the moving
// items: their mean colour is mixed over the pixel's background by that much
// (at most 1). The background is the pixel's own colour where it is
// uncovered; on a covered pixel it is unknown and estimated from the
// uncovered taps, which is what makes the smear soft on both sides of the
// silhouette. A pixel no covered tap reaches is returned unchanged, bit for
// bit.

layout(set = 0, binding = 0) uniform sampler2D sourceImage;    // HDR scene copy
layout(set = 0, binding = 1) uniform sampler2D velocityImage;  // EncodeBlur()
layout(set = 0, binding = 2) uniform sampler2D tileMaxImage;   // rg: pixels

// Must match BlurConstants in src/render/vulkan/passes/motion_blur_pass.cc.
layout(push_constant) uniform BlurConstants {
    float maxLength;  // Longest blur vector in pixels (DecodeBlur).
    int tileSize;     // Pixels per tile side of tileMaxImage.
} params;

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

const int kTapCount = 12;
// Blur vectors shorter than this (in pixels) are not worth gathering for;
// above the 10-bit encoding error of a still point (DecodeBlur()).
const float kMinGatherLength = 0.25;

// Returns the longest blur vector of the tiles around `pixel`.
vec2 NeighbourMax(ivec2 pixel)
{
    const ivec2 tile = pixel / params.tileSize;
    const ivec2 lastTile = textureSize(tileMaxImage, 0) - 1;
    vec2 longest = vec2(0.0);
    float longestSq = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            const vec2 blur = texelFetch(tileMaxImage, clamp(tile + ivec2(x, y), ivec2(0), lastTile), 0).xy;
            const float lengthSq = dot(blur, blur);
            if (lengthSq > longestSq) {
                longest = blur;
                longestSq = lengthSq;
            }
        }
    }
    return longest;
}

// Interleaved gradient noise (Jimenez 2014): a per-pixel value in [0, 1)
// that offsets the taps, trading banding for fine noise.
float TapJitter(ivec2 pixel)
{
    return fract(52.9829189 * fract(dot(vec2(pixel), vec2(0.06711056, 0.00583715))));
}

void main()
{
    const ivec2 pixel = ivec2(gl_FragCoord.xy);
    const vec4 center = texelFetch(sourceImage, pixel, 0);
    const vec2 dominant = NeighbourMax(pixel);
    const float dominantLength = length(dominant);
    if (!(dominantLength >= kMinGatherLength)) {
        outColor = center;
        return;
    }

    const vec2 direction = dominant / dominantLength;
    // Taps cover [-reach, reach] along the direction, one per `spacing`.
    const float reach = dominantLength * 0.5;
    const float spacing = 2.0 * reach / float(kTapCount);
    const ivec2 lastPixel = textureSize(sourceImage, 0) - 1;

    vec3 foregroundSum = vec3(0.0);
    float foregroundWeight = 0.0;
    vec3 backgroundSum = vec3(0.0);
    float backgroundWeight = 0.0;

    const vec4 centerVelocity = texelFetch(velocityImage, pixel, 0);
    const bool centerCovered = IsCovered(centerVelocity);
    if (centerCovered) {
        // A covered pixel is its own tap, one pixel wide.
        const float smear = max(length(DecodeBlur(centerVelocity, params.maxLength)), 1.0);
        foregroundSum = SanitizeRadiance(center.rgb) / smear;
        foregroundWeight = 1.0 / smear;
    }

    const float jitter = TapJitter(pixel);
    for (int i = 0; i < kTapCount; ++i) {
        const float offset = -reach + (float(i) + jitter) * spacing;
        const ivec2 tapPixel = clamp(ivec2(floor(gl_FragCoord.xy + direction * offset)), ivec2(0), lastPixel);
        if (tapPixel == pixel) {
            continue;
        }
        const vec4 tapVelocity = texelFetch(velocityImage, tapPixel, 0);
        const vec3 tapColor = SanitizeRadiance(texelFetch(sourceImage, tapPixel, 0).rgb);
        if (IsCovered(tapVelocity)) {
            // The tap's smear covers this pixel if it is within half its
            // length; the tap stands for `spacing` pixels of that smear (at
            // most its whole length), spread over the length.
            const float smear = max(length(DecodeBlur(tapVelocity, params.maxLength)), 1.0);
            if (abs(offset) <= smear * 0.5) {
                const float weight = min(spacing, smear) / smear;
                foregroundSum += tapColor * weight;
                foregroundWeight += weight;
            }
        } else {
            backgroundSum += tapColor;
            backgroundWeight += 1.0;
        }
    }

    if (!(foregroundWeight > 0.0)) {
        outColor = center;  // Nothing moving reaches this pixel.
        return;
    }
    const vec3 foreground = foregroundSum / foregroundWeight;
    vec3 background = SanitizeRadiance(center.rgb);
    if (centerCovered) {
        background = backgroundWeight > 0.0 ? backgroundSum / backgroundWeight : foreground;
    }
    const float coverage = min(foregroundWeight, 1.0);
    outColor = vec4(SanitizeRadiance(mix(background, foreground, coverage)), center.a);
}
