// Weapon motion blur reconstruction (render MotionBlurPass), after McGuire
// et al. 2012, "A Reconstruction Filter for Plausible Motion Blur", reduced
// to one moving layer in front of a static background. Coverage replaces
// their depth comparison: a covered pixel (a moving item) is always in front
// of an uncovered one. Program: motion_blur.
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

#include "fullscreen.slang"
#include "motion_blur.slang"
#include "sanitize.slang"

BLUE_TEXTURE(Texture2D<float4>, sourceImage, 0, BLUE_INPUT_SET);    // HDR scene copy
BLUE_TEXTURE(Texture2D<float4>, velocityImage, 1, BLUE_INPUT_SET);  // EncodeBlur()
BLUE_TEXTURE(Texture2D<float4>, tileMaxImage, 2, BLUE_INPUT_SET);   // rg: pixels

// Must match BlurConstants in src/render/vulkan/passes/motion_blur_pass.cc.
struct BlurConstants {
    float maxLength;  // Longest blur vector in pixels (DecodeBlur).
    int tileSize;     // Pixels per tile side of tileMaxImage.
};
BLUE_PUSH_CONSTANTS(BlurConstants, params);

static const int kTapCount = 12;
// Blur vectors shorter than this (in pixels) are not worth gathering for;
// above the 10-bit encoding error of a still point (DecodeBlur()).
static const float kMinGatherLength = 0.25;

// Returns the last texel (size - 1) of `image`.
int2 LastTexel(Texture2D<float4> image)
{
    uint width;
    uint height;
    image.GetDimensions(width, height);
    return int2(int(width), int(height)) - 1;
}

// Returns the longest blur vector of the tiles around `pixel`.
float2 NeighbourMax(int2 pixel)
{
    const int2 tile = pixel / params.tileSize;
    const int2 lastTile = LastTexel(tileMaxImage);
    float2 longest = float2(0.0);
    float longestSq = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            const int2 texel = clamp(tile + int2(x, y), int2(0), lastTile);
            const float2 blur = tileMaxImage.Load(int3(texel, 0)).xy;
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
float TapJitter(int2 pixel)
{
    return frac(52.9829189 * frac(dot(float2(pixel), float2(0.06711056, 0.00583715))));
}

[shader("fragment")]
float4 FragmentMain(FullscreenVaryings input) : SV_Target
{
    const float2 fragCoord = input.position.xy;
    const int2 pixel = int2(fragCoord);
    const float4 center = sourceImage.Load(int3(pixel, 0));
    const float2 dominant = NeighbourMax(pixel);
    const float dominantLength = length(dominant);
    if (!(dominantLength >= kMinGatherLength)) {
        return center;
    }

    const float2 direction = dominant / dominantLength;
    // Taps cover [-reach, reach] along the direction, one per `spacing`.
    const float reach = dominantLength * 0.5;
    const float spacing = 2.0 * reach / float(kTapCount);
    const int2 lastPixel = LastTexel(sourceImage);

    float3 foregroundSum = float3(0.0);
    float foregroundWeight = 0.0;
    float3 backgroundSum = float3(0.0);
    float backgroundWeight = 0.0;

    const float4 centerVelocity = velocityImage.Load(int3(pixel, 0));
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
        const int2 tapPixel =
            clamp(int2(floor(fragCoord + direction * offset)), int2(0), lastPixel);
        if (all(tapPixel == pixel)) {
            continue;
        }
        const float4 tapVelocity = velocityImage.Load(int3(tapPixel, 0));
        const float3 tapColor = SanitizeRadiance(sourceImage.Load(int3(tapPixel, 0)).rgb);
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
        return center;  // Nothing moving reaches this pixel.
    }
    const float3 foreground = foregroundSum / foregroundWeight;
    float3 background = SanitizeRadiance(center.rgb);
    if (centerCovered) {
        background = backgroundWeight > 0.0 ? backgroundSum / backgroundWeight : foreground;
    }
    const float coverage = min(foregroundWeight, 1.0);
    return float4(SanitizeRadiance(lerp(background, foreground, coverage)), center.a);
}
