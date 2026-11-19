#include <metal_stdlib>

#include "bindings.h"

using namespace metal;

#include "sanitize.h"

// Bloom mix, exposure and tonemapping: HDR scene radiance -> display-referred
// colour.
// Writes sRGB-encoded colour to a UNORM target (what FXAA wants to read)
// with its luma in alpha.

// Must match TonemapConstants in src/render/passes/tonemap_pass.mm.
struct TonemapConstants {
    float exposureScale;  // 2^EV
    int tonemapper;       // render::Tonemapper
    float bloomStrength;  // 0: no bloom (bloomImage not read)
    float bloomScale;     // 1 / number of bloom levels
};

// Must match FullscreenVaryings in fullscreen.vert.metal.
struct FullscreenVaryings {
    float4 position [[position]];  // Window coordinates (gl_FragCoord).
    float2 uv [[user(loc0)]];
};

constant int kAcesFitted = 0;
constant int kPbrNeutral = 1;

// Scales input to the ACES fit so it maps 0.18 to 0.18, like the other
// curves: Hill's fit alone darkens mid grey to about 0.11.
constant float kAcesExposureMatch = 1.46;

// sRGB => ACES AP1 with the RRT saturation (Stephen Hill's ACES fit,
// BakingLab). MSL matrices are built from columns: these are the transposes
// of the HLSL rows.
constant float3x3 kAcesInput = float3x3(
    float3(0.59719, 0.07600, 0.02840),
    float3(0.35458, 0.90834, 0.13383),
    float3(0.04823, 0.01566, 0.83777));
// ODT_SAT => XYZ => D60_2_D65 => sRGB.
constant float3x3 kAcesOutput = float3x3(
    float3( 1.60475, -0.10208, -0.00327),
    float3(-0.53108,  1.10813, -0.07276),
    float3(-0.07367, -0.00605,  1.07602));

static float3 RrtAndOdtFit(float3 v)
{
    const float3 a = v * (v + 0.0245786) - 0.000090537;
    const float3 b = v * (0.983729 * v + 0.4329510) + 0.238081;
    return a / b;
}

static float3 AcesFitted(float3 color)
{
    color = kAcesInput * (color * kAcesExposureMatch);
    color = RrtAndOdtFit(color);
    return clamp(kAcesOutput * color, 0.0, 1.0);
}

// Khronos PBR Neutral tone mapper (reference implementation): leaves colours
// below 0.8 alone apart from a small toe offset, compresses highlights
// towards white.
static float3 PbrNeutral(float3 color)
{
    const float startCompression = 0.8 - 0.04;
    const float desaturation = 0.15;

    const float x = min(color.r, min(color.g, color.b));
    const float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    color -= offset;

    const float peak = max(color.r, max(color.g, color.b));
    if (peak < startCompression) {
        return color;
    }
    const float d = 1.0 - startCompression;
    const float newPeak = 1.0 - d * d / (peak + d - startCompression);
    color *= newPeak / peak;
    const float g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
    return mix(color, float3(newPeak), g);
}

static float3 LinearToSrgb(float3 linear)
{
    const float3 low = linear * 12.92;
    const float3 high = 1.055 * pow(linear, float3(1.0 / 2.4)) - 0.055;
    return select(high, low, linear <= float3(0.0031308));
}

fragment float4 FragmentMain(FullscreenVaryings varyings [[stage_in]],
                             texture2d<float> hdrImage [[texture(kTextureInput0)]],
                             // Mip 0 of the bloom chain, half resolution: the
                             // sum of every level.
                             texture2d<float> bloomImage [[texture(kTextureInput0 + 1)]],
                             sampler inputSampler [[sampler(kSamplerInput)]],
                             constant TonemapConstants& params [[buffer(kBufferPass)]])
{
    // Same size as the target: fetch the exact texel.
    // Non-finite inputs are black, never NaN through the curves below.
    float3 hdr = SanitizeRadiance(hdrImage.read(uint2(varyings.position.xy)).rgb);
    if (params.bloomStrength > 0.0) {
        // Energy-preserving: a fraction of every pixel is replaced by the
        // blurred image (the average of the levels).
        const float3 bloom = SanitizeRadiance(bloomImage.sample(inputSampler, varyings.uv).rgb * params.bloomScale);
        hdr = mix(hdr, bloom, params.bloomStrength);
    }
    const float3 exposed = hdr * params.exposureScale;

    float3 mapped;
    if (params.tonemapper == kAcesFitted) {
        mapped = AcesFitted(exposed);
    } else if (params.tonemapper == kPbrNeutral) {
        mapped = PbrNeutral(exposed);
    } else {
        mapped = exposed;
    }
    const float3 encoded = LinearToSrgb(clamp(mapped, 0.0, 1.0));
    const float luma = dot(encoded, float3(0.299, 0.587, 0.114));
    return float4(encoded, luma);
}
