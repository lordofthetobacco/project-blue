// Guards against non-finite values (NaN / Inf) in colour and direction math,
// shared by every pass that writes or reads HDR radiance. Included with
// #include "sanitize.h" after <metal_stdlib>; safe to include more than once.
//
// One NaN or Inf pixel in the HDR scene image is enough to ruin a frame: the
// bloom down-sample spreads it through ever smaller mips (Inf - Inf and
// Inf * 0 are NaN), the composite mixes it back as big square blocks, and
// the tonemapper turns NaN into black. So the scene passes write finite
// radiance only, and the post passes treat a broken input as black.
//
// isnan() is spelled out because min / max / clamp are not guaranteed to
// handle NaN operands the same way on every GPU: some return the other
// operand, others propagate the NaN. The shader compiler runs with
// MTLMathModeSafe, which keeps isnan() / isinf() from being folded away.
//
// Functions are qualified with metal:: so that the header does not depend on
// the including file's `using namespace metal;`.
#ifndef BLUE_SHADERS_METAL_SANITIZE_H_
#define BLUE_SHADERS_METAL_SANITIZE_H_

// Largest radiance (before exposure) a scene pass writes to the RGBA16F HDR
// target. The analytic sun is ~3.3 (scene/default_lighting.h): a sunlit white
// surface is ~3, so 4096 sits about 10 stops above it and every real
// highlight still saturates the tonemapper and blooms. Only the GGX peak of
// the sun on a near-mirror goes past it (D ~ 1 / (pi alpha^2) ~ 7.8e4 at the
// minimum roughness, ~2e5 in total, which is +Inf in half floats). The 16x
// margin to the fp16 maximum (65504) keeps the bloom chain finite: its
// up-sample adds every level onto mip 0, at most
// render::RenderTargets::kMaxBloomLevels (6) of them.
constant float kMaxSceneRadiance = 4096.0;
// Largest finite value written to a half-float environment cube.
constant float kMaxHalfRadiance = 65000.0;

// Returns `color` with NaN components replaced by 0 and every component
// clamped to [0, maxValue] (+Inf becomes maxValue, -Inf 0).
static inline float3 SanitizeColor(float3 color, float maxValue)
{
    color = metal::select(color, float3(0.0), metal::isnan(color));
    return metal::clamp(color, float3(0.0), float3(maxValue));
}

static inline float2 SanitizeColor(float2 color, float maxValue)
{
    color = metal::select(color, float2(0.0), metal::isnan(color));
    return metal::clamp(color, float2(0.0), float2(maxValue));
}

static inline float SanitizeColor(float value, float maxValue)
{
    return metal::isnan(value) ? 0.0 : metal::clamp(value, 0.0, maxValue);
}

// Scene radiance as the HDR target may hold it.
static inline float3 SanitizeRadiance(float3 color)
{
    return SanitizeColor(color, kMaxSceneRadiance);
}

// Returns `v` normalized, or `fallback` (unit) when `v` is too short or not
// finite to have a direction (normalize(float3(0)) is NaN).
static inline float3 SafeNormalize(float3 v, float3 fallback)
{
    const float lengthSq = metal::dot(v, v);
    return lengthSq > 1.0e-20 && !metal::isinf(lengthSq) ? v * metal::rsqrt(lengthSq) : fallback;
}

#endif  // BLUE_SHADERS_METAL_SANITIZE_H_
