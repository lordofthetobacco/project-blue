// Metallic-roughness PBR shading: Cook-Torrance specular (GGX distribution,
// height-correlated Smith visibility, Schlick Fresnel) plus Lambert diffuse.
// Included with #include "pbr.glsl" after common.glsl.
//
// Light intensity convention (see src/render/scene_view.h): a light's color *
// intensity is the radiance a white Lambertian surface facing it reflects,
// so its irradiance is pi times that. DirectLight() applies the factor.

#include "environment.glsl"

const float kPi = 3.14159265359;
// Below this perceptual roughness GGX highlights get too small for fp16 and
// for the pixel grid.
const float kMinPerceptualRoughness = 0.045;
// Dielectric reflectance at normal incidence (4%).
const vec3 kDielectricF0 = vec3(0.04);
// Geometric specular anti-aliasing (Tokuyoshi & Kaplanyan 2019, as in
// Filament): screen-space variance of the normal widens the GGX lobe.
const float kSpecularAaVariance = 0.15;
const float kSpecularAaThreshold = 0.2;

// Everything the lighting functions need about one shaded point.
struct Surface {
    vec3 position;          // World space.
    vec3 normal;            // Shading normal (normal map applied), unit.
    vec3 view;              // Towards the camera, unit.
    float NdotV;            // Clamped away from 0.
    vec3 diffuseColor;      // base * (1 - metallic)
    vec3 f0;                // mix(0.04, base, metallic)
    float roughness;        // Perceptual, filtered and clamped.
    float alpha;            // roughness^2 (GGX alpha).
    float occlusion;        // Ambient occlusion, [0, 1].
};

// Returns `perceptualRoughness` widened by the curvature of `normal` across
// the pixel, which keeps sharp highlights on bumpy or curved surfaces from
// sparkling. Must run in uniform control flow (derivatives).
float FilterRoughness(float perceptualRoughness, vec3 normal)
{
    const vec3 du = dFdx(normal);
    const vec3 dv = dFdy(normal);
    const float variance = kSpecularAaVariance * (dot(du, du) + dot(dv, dv));
    const float alpha = perceptualRoughness * perceptualRoughness;
    const float kernel = min(2.0 * variance, kSpecularAaThreshold);
    const float filteredAlpha = clamp(alpha * alpha + kernel, 0.0, 1.0);
    return sqrt(sqrt(filteredAlpha));
}

// Returns a Surface from the material inputs at one point.
Surface MakeSurface(vec3 position, vec3 normal, vec3 baseColor, float metallic,
                    float perceptualRoughness, float occlusion)
{
    Surface s;
    s.position = position;
    s.normal = normal;
    s.view = normalize(frame.cameraPosition.xyz - position);
    s.NdotV = max(dot(normal, s.view), 1.0e-4);
    s.diffuseColor = baseColor * (1.0 - metallic);
    s.f0 = mix(kDielectricF0, baseColor, metallic);
    s.roughness = clamp(perceptualRoughness, kMinPerceptualRoughness, 1.0);
    s.alpha = s.roughness * s.roughness;
    s.occlusion = occlusion;
    return s;
}

float DistributionGgx(float NdotH, float alpha)
{
    const float a2 = alpha * alpha;
    const float d = (NdotH * a2 - NdotH) * NdotH + 1.0;
    return a2 / (kPi * d * d);
}

// Height-correlated Smith visibility, G / (4 NdotL NdotV) folded in.
float VisibilitySmithGgxCorrelated(float NdotV, float NdotL, float alpha)
{
    const float a2 = alpha * alpha;
    const float ggxV = NdotL * sqrt(NdotV * NdotV * (1.0 - a2) + a2);
    const float ggxL = NdotV * sqrt(NdotL * NdotL * (1.0 - a2) + a2);
    return 0.5 / max(ggxV + ggxL, 1.0e-7);
}

vec3 FresnelSchlick(vec3 f0, float VdotH)
{
    const float f = pow(1.0 - VdotH, 5.0);
    return f + f0 * (1.0 - f);
}

// Returns the light reflected towards the camera from a light in direction
// `toLight` (unit) with `intensity` (color * intensity / attenuation, see the
// convention above).
vec3 DirectLight(Surface s, vec3 toLight, vec3 intensity)
{
    const float NdotL = dot(s.normal, toLight);
    if (NdotL <= 0.0) {
        return vec3(0.0);
    }
    const vec3 halfway = normalize(s.view + toLight);
    const float NdotH = clamp(dot(s.normal, halfway), 0.0, 1.0);
    const float VdotH = clamp(dot(s.view, halfway), 0.0, 1.0);

    const float D = DistributionGgx(NdotH, s.alpha);
    const float V = VisibilitySmithGgxCorrelated(s.NdotV, NdotL, s.alpha);
    const vec3 F = FresnelSchlick(s.f0, VdotH);
    const vec3 specular = D * V * F;
    const vec3 diffuse = s.diffuseColor / kPi;
    return (diffuse + specular) * (kPi * intensity) * NdotL;
}

// Analytic fit of the split-sum BRDF integral (Karis, "Physically Based
// Shading on Mobile"): returns (scale, bias) for f0.
vec2 EnvironmentBrdfApprox(float NdotV, float roughness)
{
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    const vec4 r = roughness * c0 + c1;
    const float a004 = min(r.x * r.x, exp2(-9.28 * NdotV)) * r.x + r.y;
    return vec2(-1.04, 1.04) * a004 + r.zw;
}

// Returns the irradiance (rgb) from the environment's SH9 at unit world
// normal `n` (lighting::IrradianceSh9 convention), before intensity.
vec3 EnvironmentIrradiance(vec3 n)
{
    const vec4 c0 = frame.irradianceSh[0];
    vec3 e = c0.rgb;
    e += frame.irradianceSh[1].rgb * n.y;
    e += frame.irradianceSh[2].rgb * n.z;
    e += frame.irradianceSh[3].rgb * n.x;
    e += frame.irradianceSh[4].rgb * (n.x * n.y);
    e += frame.irradianceSh[5].rgb * (n.y * n.z);
    e += frame.irradianceSh[6].rgb * (3.0 * n.z * n.z - 1.0);
    e += frame.irradianceSh[7].rgb * (n.x * n.z);
    e += frame.irradianceSh[8].rgb * (n.x * n.x - n.y * n.y);
    return max(e, vec3(0.0));
}

// Specular occlusion from ambient occlusion (Lagarde and de Rousiers 2014).
float SpecularOcclusion(float NdotV, float ao, float roughness)
{
    return clamp(pow(NdotV + ao, exp2(-16.0 * roughness - 1.0)) - 1.0 + ao, 0.0, 1.0);
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
vec3 AmbientLight(Surface s, vec3 geometricNormal)
{
    const vec2 screenUv = gl_FragCoord.xy * frame.screen.zw;
    const float screenAo = textureLod(ambientOcclusionMap, screenUv, 0.0).r;
    const float ao = s.occlusion * screenAo;
    if (frame.environmentParams.w < 0.5) {
        const vec2 brdf = EnvironmentBrdfApprox(s.NdotV, s.roughness);
        const vec3 specular = s.f0 * brdf.x + brdf.y;
        return frame.ambientColor.rgb * (s.diffuseColor + specular) * ao;
    }
    const float intensity = frame.environmentParams.x;
    const float rotation = -frame.environmentParams.y;

    const vec2 lut = textureLod(brdfLut, vec2(s.NdotV, s.roughness), 0.0).rg;
    const vec3 singleScatter = s.f0 * lut.x + lut.y;  // F_ss * E_ss
    const float singleEnergy = lut.x + lut.y;          // E_ss
    const float multiEnergy = 1.0 - singleEnergy;      // E_ms
    const vec3 averageFresnel = s.f0 + (1.0 - s.f0) / 21.0;
    const vec3 multiScatter = singleScatter * averageFresnel /
                              (1.0 - multiEnergy * averageFresnel);  // F_ms
    // Diffuse gets what neither specular term reflects.
    const vec3 diffuseWeight =
        s.diffuseColor * (1.0 - singleScatter - multiScatter * multiEnergy);

    const vec3 irradiance =
        EnvironmentIrradiance(RotateY(s.normal, rotation)) * (intensity / kPi);

    const vec3 reflected = reflect(-s.view, s.normal);
    const float lod = s.roughness * frame.environmentParams.z;
    const vec3 prefiltered =
        textureLod(environmentMap, RotateY(reflected, rotation), lod).rgb * intensity;
    // Fade reflections that point into the surface (normal-mapped grazing
    // angles), which would otherwise show the ground or sky below it.
    float horizon = clamp(1.0 + dot(reflected, geometricNormal), 0.0, 1.0);
    horizon *= horizon;
    const float specularAo = SpecularOcclusion(s.NdotV, ao, s.roughness) * horizon;

    return singleScatter * prefiltered * specularAo +
           (multiScatter * multiEnergy * specularAo + diffuseWeight * ao) * irradiance;
}

// Same without a separate geometric normal (no horizon fade).
vec3 AmbientLight(Surface s)
{
    return AmbientLight(s, s.normal);
}
