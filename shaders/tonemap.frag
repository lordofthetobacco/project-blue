#version 450

// Bloom mix, exposure and tonemapping: HDR scene radiance -> display-referred
// colour.
// Writes sRGB-encoded colour to a UNORM target (what FXAA wants to read)
// with its luma in alpha.

#include "sanitize.glsl"

layout(set = 0, binding = 0) uniform sampler2D hdrImage;
// Mip 0 of the bloom chain, half resolution: the sum of every level.
layout(set = 0, binding = 1) uniform sampler2D bloomImage;

// Must match TonemapConstants in src/render/passes/tonemap_pass.cc.
layout(push_constant) uniform TonemapConstants {
    float exposureScale;  // 2^EV
    int tonemapper;       // render::Tonemapper
    float bloomStrength;  // 0: no bloom (bloomImage not read)
    float bloomScale;     // 1 / number of bloom levels
} params;

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

const int kAcesFitted = 0;
const int kPbrNeutral = 1;

// Scales input to the ACES fit so it maps 0.18 to 0.18, like the other
// curves: Hill's fit alone darkens mid grey to about 0.11.
const float kAcesExposureMatch = 1.46;

// sRGB => ACES AP1 with the RRT saturation (Stephen Hill's ACES fit,
// BakingLab). GLSL matrices are column-major: these are the transposes of
// the HLSL rows.
const mat3 kAcesInput = mat3(
    0.59719, 0.07600, 0.02840,
    0.35458, 0.90834, 0.13383,
    0.04823, 0.01566, 0.83777);
// ODT_SAT => XYZ => D60_2_D65 => sRGB.
const mat3 kAcesOutput = mat3(
     1.60475, -0.10208, -0.00327,
    -0.53108,  1.10813, -0.07276,
    -0.07367, -0.00605,  1.07602);

vec3 RrtAndOdtFit(vec3 v)
{
    const vec3 a = v * (v + 0.0245786) - 0.000090537;
    const vec3 b = v * (0.983729 * v + 0.4329510) + 0.238081;
    return a / b;
}

vec3 AcesFitted(vec3 color)
{
    color = kAcesInput * (color * kAcesExposureMatch);
    color = RrtAndOdtFit(color);
    return clamp(kAcesOutput * color, 0.0, 1.0);
}

// Khronos PBR Neutral tone mapper (reference implementation): leaves colours
// below 0.8 alone apart from a small toe offset, compresses highlights
// towards white.
vec3 PbrNeutral(vec3 color)
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
    return mix(color, vec3(newPeak), g);
}

vec3 LinearToSrgb(vec3 linear)
{
    const vec3 low = linear * 12.92;
    const vec3 high = 1.055 * pow(linear, vec3(1.0 / 2.4)) - 0.055;
    return mix(high, low, lessThanEqual(linear, vec3(0.0031308)));
}

void main()
{
    // Same size as the target: fetch the exact texel.
    // Non-finite inputs are black, never NaN through the curves below.
    vec3 hdr = SanitizeRadiance(texelFetch(hdrImage, ivec2(gl_FragCoord.xy), 0).rgb);
    if (params.bloomStrength > 0.0) {
        // Energy-preserving: a fraction of every pixel is replaced by the
        // blurred image (the average of the levels).
        const vec3 bloom = SanitizeRadiance(texture(bloomImage, inUv).rgb * params.bloomScale);
        hdr = mix(hdr, bloom, params.bloomStrength);
    }
    const vec3 exposed = hdr * params.exposureScale;

    vec3 mapped;
    if (params.tonemapper == kAcesFitted) {
        mapped = AcesFitted(exposed);
    } else if (params.tonemapper == kPbrNeutral) {
        mapped = PbrNeutral(exposed);
    } else {
        mapped = exposed;
    }
    const vec3 encoded = LinearToSrgb(clamp(mapped, 0.0, 1.0));
    const float luma = dot(encoded, vec3(0.299, 0.587, 0.114));
    outColor = vec4(encoded, luma);
}
