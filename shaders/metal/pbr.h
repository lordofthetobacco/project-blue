// Metallic-roughness PBR shading: Cook-Torrance specular (GGX distribution,
// height-correlated Smith visibility, Schlick Fresnel) plus Lambert diffuse.
// Included with #include "pbr.h" after <metal_stdlib>, "bindings.h" and the
// using-directive (see mesh.frag.metal); it includes common.h,
// environment.h and sanitize.h itself. Helpers that read the frame's
// resources take `thread const FrameResources&` (common.h).
//
// Light intensity convention (see src/render/scene_view.h): a light's color *
// intensity is the radiance a white Lambertian surface facing it reflects,
// so its irradiance is pi times that. DirectLight() applies the factor.
//
// Every function here is finite and non-negative for all legal inputs
// (roughness 0..1, metallic 0..1, any directions, grazing and back-facing
// shading normals). src/render/brdf_reference.h mirrors the math 1:1 under
// the same names for the CPU test (tests/brdf_reference_test.cc); keep the
// two in sync.
#ifndef BLUE_SHADERS_METAL_PBR_H_
#define BLUE_SHADERS_METAL_PBR_H_

#include "common.h"
#include "environment.h"
#include "sanitize.h"

constant float kPi = 3.14159265359;
// Below this perceptual roughness GGX highlights get too small for fp16 and
// for the pixel grid.
constant float kMinPerceptualRoughness = 0.045;
// Dielectric reflectance at normal incidence (4%).
constant float3 kDielectricF0 = float3(0.04);
// Geometric specular anti-aliasing (Tokuyoshi & Kaplanyan 2019, as in
// Filament): screen-space variance of the normal widens the GGX lobe.
constant float kSpecularAaVariance = 0.15;
constant float kSpecularAaThreshold = 0.2;

// Everything the lighting functions need about one shaded point.
struct Surface {
    float3 position;        // World space.
    float3 normal;          // Shading normal (normal map applied), unit.
    float3 view;            // Towards the camera, unit.
    float NdotV;            // Clamped away from 0.
    float3 diffuseColor;    // base * (1 - metallic)
    float3 f0;              // mix(0.04, base, metallic)
    float roughness;        // Perceptual, filtered and clamped.
    float alpha;            // roughness^2 (GGX alpha).
    float occlusion;        // Ambient occlusion, [0, 1].
};

// Returns `perceptualRoughness` widened by the curvature of `normal` across
// the pixel, which keeps sharp highlights on bumpy or curved surfaces from
// sparkling. Must run in uniform control flow (derivatives).
static inline float FilterRoughness(float perceptualRoughness, float3 normal)
{
    const float3 du = metal::dfdx(normal);
    const float3 dv = metal::dfdy(normal);
    const float variance = kSpecularAaVariance * (metal::dot(du, du) + metal::dot(dv, dv));
    const float alpha = perceptualRoughness * perceptualRoughness;
    // Not `kernel`: a reserved word in MSL.
    const float filterKernel = metal::min(2.0 * variance, kSpecularAaThreshold);
    const float filteredAlpha = metal::clamp(alpha * alpha + filterKernel, 0.0, 1.0);
    return metal::sqrt(metal::sqrt(filteredAlpha));
}

// Returns a Surface from the material inputs at one point.
static inline Surface MakeSurface(thread const FrameResources& res, float3 position,
                                  float3 normal, float3 baseColor, float metallic,
                                  float perceptualRoughness, float occlusion)
{
    Surface s;
    s.position = position;
    s.normal = normal;
    s.view = SafeNormalize(res.frame->cameraPosition.xyz - position, normal);
    s.NdotV = metal::clamp(metal::dot(normal, s.view), 1.0e-4, 1.0);
    s.diffuseColor = baseColor * (1.0 - metallic);
    // Reflectance above 1 would make the multiple-scattering term blow up.
    s.f0 = metal::clamp(metal::mix(kDielectricF0, baseColor, float3(metallic)), float3(0.0),
                        float3(1.0));
    s.roughness = metal::clamp(perceptualRoughness, kMinPerceptualRoughness, 1.0);
    s.alpha = s.roughness * s.roughness;
    s.occlusion = occlusion;
    return s;
}

static inline float DistributionGgx(float NdotH, float alpha)
{
    const float a2 = alpha * alpha;
    // d = NdotH^2 (a2 - 1) + 1 lies in [a2, 1]; rounding near NdotH = 1 must
    // not take it below.
    const float d = metal::max((NdotH * a2 - NdotH) * NdotH + 1.0, a2);
    return a2 / (kPi * d * d);
}

// Height-correlated Smith visibility, G / (4 NdotL NdotV) folded in.
static inline float VisibilitySmithGgxCorrelated(float NdotV, float NdotL, float alpha)
{
    const float a2 = alpha * alpha;
    const float ggxV = NdotL * metal::sqrt(metal::max(NdotV * NdotV * (1.0 - a2) + a2, 0.0));
    const float ggxL = NdotV * metal::sqrt(metal::max(NdotL * NdotL * (1.0 - a2) + a2, 0.0));
    return 0.5 / metal::max(ggxV + ggxL, 1.0e-7);
}

static inline float3 FresnelSchlick(float3 f0, float VdotH)
{
    const float f = metal::pow(metal::clamp(1.0 - VdotH, 0.0, 1.0), 5.0);
    return f + f0 * (1.0 - f);
}

// Returns the light reflected towards the camera from a light in direction
// `toLight` (unit) with `intensity` (color * intensity / attenuation, see the
// convention above).
static inline float3 DirectLight(thread const Surface& s, float3 toLight, float3 intensity)
{
    const float NdotL = metal::min(metal::dot(s.normal, toLight), 1.0);
    if (!(NdotL > 0.0)) {
        return float3(0.0);  // Also a NaN light direction.
    }
    // L = -V (light straight behind the camera through a bumped normal)
    // has no halfway vector; the normal is the limit of the others.
    const float3 halfway = SafeNormalize(s.view + toLight, s.normal);
    const float NdotH = metal::clamp(metal::dot(s.normal, halfway), 0.0, 1.0);
    const float VdotH = metal::clamp(metal::dot(s.view, halfway), 0.0, 1.0);

    const float D = DistributionGgx(NdotH, s.alpha);
    const float V = VisibilitySmithGgxCorrelated(s.NdotV, NdotL, s.alpha);
    const float3 F = FresnelSchlick(s.f0, VdotH);
    const float3 specular = D * V * F;
    const float3 diffuse = s.diffuseColor / kPi;
    return (diffuse + specular) * (kPi * intensity) * NdotL;
}

// Analytic fit of the split-sum BRDF integral (Karis, "Physically Based
// Shading on Mobile"): returns (scale, bias) for f0.
static inline float2 EnvironmentBrdfApprox(float NdotV, float roughness)
{
    const float4 c0 = float4(-1.0, -0.0275, -0.572, 0.022);
    const float4 c1 = float4(1.0, 0.0425, 1.04, -0.04);
    const float4 r = roughness * c0 + c1;
    const float a004 = metal::min(r.x * r.x, metal::exp2(-9.28 * NdotV)) * r.x + r.y;
    return float2(-1.04, 1.04) * a004 + r.zw;
}

// Returns the irradiance (rgb) from the environment's SH9 at unit world
// normal `n` (lighting::IrradianceSh9 convention), before intensity.
static inline float3 EnvironmentIrradiance(thread const FrameResources& res, float3 n)
{
    constant FrameUniforms& frame = *res.frame;
    const float4 c0 = frame.irradianceSh[0];
    float3 e = c0.rgb;
    e += frame.irradianceSh[1].rgb * n.y;
    e += frame.irradianceSh[2].rgb * n.z;
    e += frame.irradianceSh[3].rgb * n.x;
    e += frame.irradianceSh[4].rgb * (n.x * n.y);
    e += frame.irradianceSh[5].rgb * (n.y * n.z);
    e += frame.irradianceSh[6].rgb * (3.0 * n.z * n.z - 1.0);
    e += frame.irradianceSh[7].rgb * (n.x * n.z);
    e += frame.irradianceSh[8].rgb * (n.x * n.x - n.y * n.y);
    return metal::max(e, float3(0.0));
}

// Specular occlusion from ambient occlusion (Lagarde and de Rousiers 2014).
// At the minimum roughness the exponent is ~0.3, so at NdotV = 1 the result
// is ~ao: a mirror is occluded about as strongly as the diffuse term.
static inline float SpecularOcclusion(float NdotV, float ao, float roughness)
{
    const float base = metal::max(NdotV + ao, 0.0);
    return metal::clamp(metal::pow(base, metal::exp2(-16.0 * roughness - 1.0)) - 1.0 + ao,
                        0.0, 1.0);
}

// Returns the split-sum (scale, bias) for `s` from the LUT, with the
// analytic fit standing in for a texel that is not finite.
static inline float2 EnvironmentBrdf(thread const FrameResources& res, thread const Surface& s)
{
    const float2 lut =
        res.brdfLut.sample(res.linearClamp, float2(s.NdotV, s.roughness), metal::level(0.0)).rg;
    if (metal::any(metal::isnan(lut)) || metal::any(metal::isinf(lut))) {
        return EnvironmentBrdfApprox(s.NdotV, s.roughness);
    }
    return metal::clamp(lut, float2(0.0), float2(1.0));
}

// Returns the ambient light reflected by `s` with image-based lighting
// (render::Environment):
//   diffuse   base * (1 - metallic) * E / pi, E from the SH9 irradiance;
//   specular  prefiltered radiance (reflection direction, roughness -> mip)
//             * (F0 * scale + bias) from the split-sum LUT;
//   multiple scattering (Fdez-Aguera 2019): the energy single scattering
//             loses is added back, tinted by the average Fresnel, which keeps
//             rough metals from darkening;
//   occlusion the material's AO times the screen-space AO for diffuse,
//             Lagarde's specular occlusion for specular, and a horizon fade
//             for reflections pointing below the geometric surface.
// The environment's rotation and intensity apply to all of it. Without a
// bound environment the scene's flat ambient term is used instead (the
// renderer always binds one, so frame.ambientColor is unused in practice).
// `fragCoord` is the fragment's window position (its [[position]]), which
// locates the screen-space AO texel.
static inline float3 AmbientLight(thread const FrameResources& res, thread const Surface& s,
                                  float3 geometricNormal, float2 fragCoord)
{
    constant FrameUniforms& frame = *res.frame;
    const float2 screenUv = fragCoord * frame.screen.zw;
    const float screenAo =
        res.ambientOcclusionMap.sample(res.linearClamp, screenUv, metal::level(0.0)).r;
    const float ao = SanitizeColor(s.occlusion * screenAo, 1.0);
    if (frame.environmentParams.w < 0.5) {
        const float2 brdf = EnvironmentBrdfApprox(s.NdotV, s.roughness);
        const float3 specular = s.f0 * brdf.x + brdf.y;
        return frame.ambientColor.rgb * (s.diffuseColor + specular) * ao;
    }
    const float intensity = frame.environmentParams.x;
    const float rotation = -frame.environmentParams.y;

    const float2 lut = EnvironmentBrdf(res, s);
    const float3 singleScatter = s.f0 * lut.x + lut.y;          // F_ss * E_ss
    const float singleEnergy = metal::min(lut.x + lut.y, 1.0);  // E_ss
    const float multiEnergy = 1.0 - singleEnergy;               // E_ms
    const float3 averageFresnel = s.f0 + (1.0 - s.f0) / 21.0;
    // The denominator is >= E_ss for f0 <= 1; it reaches 0 only for a white
    // metal whose LUT texel reflects nothing (E_ss = 0).
    const float3 multiScatter = singleScatter * averageFresnel /
                                metal::max(1.0 - multiEnergy * averageFresnel, float3(1.0e-4));  // F_ms
    // Diffuse gets what neither specular term reflects.
    const float3 diffuseWeight =
        s.diffuseColor * metal::max(1.0 - singleScatter - multiScatter * multiEnergy, float3(0.0));

    const float3 irradiance =
        EnvironmentIrradiance(res, RotateY(s.normal, rotation)) * (intensity / kPi);

    const float3 reflected = metal::reflect(-s.view, s.normal);
    const float lod = s.roughness * frame.environmentParams.z;
    const float3 prefiltered =
        SanitizeColor(res.environmentMap.sample(res.linearClamp, RotateY(reflected, rotation),
                                                metal::level(lod)).rgb,
                      kMaxHalfRadiance) * intensity;
    // Fade reflections that point into the surface (normal-mapped grazing
    // angles), which would otherwise show the ground or sky below it.
    float horizon = metal::clamp(1.0 + metal::dot(reflected, geometricNormal), 0.0, 1.0);
    horizon *= horizon;
    const float specularAo = SpecularOcclusion(s.NdotV, ao, s.roughness) * horizon;

    return singleScatter * prefiltered * specularAo +
           (multiScatter * multiEnergy * specularAo + diffuseWeight * ao) * irradiance;
}

// Same without a separate geometric normal (no horizon fade).
static inline float3 AmbientLight(thread const FrameResources& res, thread const Surface& s,
                                  float2 fragCoord)
{
    return AmbientLight(res, s, s.normal, fragCoord);
}

#endif  // BLUE_SHADERS_METAL_PBR_H_
