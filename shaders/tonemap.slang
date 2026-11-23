// Bloom mix, exposure and tonemapping: HDR scene radiance -> display-referred
// colour (render TonemapPass). Writes sRGB-encoded colour to a UNORM target
// (what FXAA wants to read) with its luma in alpha. Program: tonemap.

#include "fullscreen.slang"
#include "sanitize.slang"

BLUE_TEXTURE(Texture2D<float4>, hdrImage, 0, BLUE_INPUT_SET);
// Mip 0 of the bloom chain, half resolution: the sum of every level.
BLUE_TEXTURE(Texture2D<float4>, bloomImage, 1, BLUE_INPUT_SET);

// Must match TonemapConstants in src/render/vulkan/passes/tonemap_pass.cc.
struct TonemapConstants {
    float exposureScale;  // 2^EV
    int tonemapper;       // render::Tonemapper
    float bloomStrength;  // 0: no bloom (bloomImage not read)
    float bloomScale;     // 1 / number of bloom levels
};
BLUE_PUSH_CONSTANTS(TonemapConstants, params);

static const int kAcesFitted = 0;
static const int kPbrNeutral = 1;

// Scales input to the ACES fit so it maps 0.18 to 0.18, like the other
// curves: Hill's fit alone darkens mid grey to about 0.11.
static const float kAcesExposureMatch = 1.46;

// sRGB => ACES AP1 with the RRT saturation (Stephen Hill's ACES fit,
// BakingLab), rows as published.
static const float3x3 kAcesInput = float3x3(
    0.59719, 0.35458, 0.04823,
    0.07600, 0.90834, 0.01566,
    0.02840, 0.13383, 0.83777);
// ODT_SAT => XYZ => D60_2_D65 => sRGB.
static const float3x3 kAcesOutput = float3x3(
     1.60475, -0.53108, -0.07367,
    -0.10208,  1.10813, -0.00605,
    -0.00327, -0.07276,  1.07602);

float3 RrtAndOdtFit(float3 v)
{
    const float3 a = v * (v + 0.0245786) - 0.000090537;
    const float3 b = v * (0.983729 * v + 0.4329510) + 0.238081;
    return a / b;
}

float3 AcesFitted(float3 color)
{
    color = mul(kAcesInput, color * kAcesExposureMatch);
    color = RrtAndOdtFit(color);
    return clamp(mul(kAcesOutput, color), 0.0, 1.0);
}

// Khronos PBR Neutral tone mapper (reference implementation): leaves colours
// below 0.8 alone apart from a small toe offset, compresses highlights
// towards white.
float3 PbrNeutral(float3 color)
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
    return lerp(color, float3(newPeak), g);
}

float3 LinearToSrgb(float3 linearColor)
{
    const float3 low = linearColor * 12.92;
    const float3 high = 1.055 * pow(linearColor, float3(1.0 / 2.4)) - 0.055;
    return select(linearColor <= float3(0.0031308), low, high);
}

[shader("fragment")]
float4 FragmentMain(FullscreenVaryings input) : SV_Target
{
    // Same size as the target: fetch the exact texel.
    // Non-finite inputs are black, never NaN through the curves below.
    float3 hdr = SanitizeRadiance(hdrImage.Load(int3(int2(input.position.xy), 0)).rgb);
    if (params.bloomStrength > 0.0) {
        // Energy-preserving: a fraction of every pixel is replaced by the
        // blurred image (the average of the levels). Level 0 is what
        // implicit LOD picks (the image is half the target's size); read
        // only here because the image is not valid while bloom is off.
        const float3 bloom = SanitizeRadiance(
            bloomImage.SampleLevel(inputSampler, input.uv, 0.0).rgb * params.bloomScale);
        hdr = lerp(hdr, bloom, params.bloomStrength);
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
