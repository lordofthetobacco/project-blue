// Guards against non-finite values (NaN / Inf) in colour and direction math,
// shared by every pass that writes or reads HDR radiance. Included with
// #include "sanitize.glsl"; safe to include more than once.
//
// One NaN or Inf pixel in the HDR scene image is enough to ruin a frame: the
// bloom down-sample spreads it through ever smaller mips (Inf - Inf and
// Inf * 0 are NaN), the composite mixes it back as big square blocks, and
// the tonemapper turns NaN into black. So the scene passes write finite
// radiance only, and the post passes treat a broken input as black.
//
// isnan() is spelled out because GLSL leaves min / max / clamp undefined for
// NaN operands (SPIR-V FMin / FMax / FClamp): some GPUs return the other
// operand, others propagate the NaN.
#ifndef BLUE_SHADERS_SANITIZE_GLSL_
#define BLUE_SHADERS_SANITIZE_GLSL_

// Largest radiance (before exposure) a scene pass writes to the RGBA16F HDR
// target. The analytic sun is ~3.3 (scene/default_lighting.h): a sunlit white
// surface is ~3, so 4096 sits about 10 stops above it and every real
// highlight still saturates the tonemapper and blooms. Only the GGX peak of
// the sun on a near-mirror goes past it (D ~ 1 / (pi alpha^2) ~ 7.8e4 at the
// minimum roughness, ~2e5 in total, which is +Inf in half floats). The 16x
// margin to the fp16 maximum (65504) keeps the bloom chain finite: its
// up-sample adds every level onto mip 0, at most
// render::RenderTargets::kMaxBloomLevels (6) of them.
const float kMaxSceneRadiance = 4096.0;
// Largest finite value written to a half-float environment cube.
const float kMaxHalfRadiance = 65000.0;

// Returns `color` with NaN components replaced by 0 and every component
// clamped to [0, maxValue] (+Inf becomes maxValue, -Inf 0).
vec3 SanitizeColor(vec3 color, float maxValue)
{
    color = mix(color, vec3(0.0), isnan(color));
    return clamp(color, vec3(0.0), vec3(maxValue));
}

vec2 SanitizeColor(vec2 color, float maxValue)
{
    color = mix(color, vec2(0.0), isnan(color));
    return clamp(color, vec2(0.0), vec2(maxValue));
}

float SanitizeColor(float value, float maxValue)
{
    return isnan(value) ? 0.0 : clamp(value, 0.0, maxValue);
}

// Scene radiance as the HDR target may hold it.
vec3 SanitizeRadiance(vec3 color)
{
    return SanitizeColor(color, kMaxSceneRadiance);
}

// Returns `v` normalized, or `fallback` (unit) when `v` is too short or not
// finite to have a direction (normalize(vec3(0)) is NaN).
vec3 SafeNormalize(vec3 v, vec3 fallback)
{
    const float lengthSq = dot(v, v);
    return lengthSq > 1.0e-20 && !isinf(lengthSq) ? v * inversesqrt(lengthSq) : fallback;
}

#endif  // BLUE_SHADERS_SANITIZE_GLSL_
