#include <metal_stdlib>

#include "bindings.h"

using namespace metal;

#include "common.h"
#include "shadows.h"
#include "pbr.h"
#include "material.h"

// Pipeline variants (render::MaterialVariant).
constant bool kAlphaMask [[function_constant(kConstantAlphaMask)]];
constant bool kDoubleSided [[function_constant(kConstantDoubleSided)]];

// Must match MeshVaryings in mesh.vert.metal.
struct MeshVaryings {
    float4 position [[position]];  // Window coordinates (gl_FragCoord).
    float3 worldPosition [[user(loc0)]];
    float3 normal [[user(loc1)]];
    float4 uv [[user(loc2)]];  // xy: uv0, zw: uv1
    float4 tangent [[user(loc3)]];
};

// Returns the shading normal: `normal` perturbed by the tangent-space normal
// `mapped` (already unpacked to [-1, 1]). The mesh's bitangent
// cross(N, T) * w points towards increasing v, which is image-down (UV
// origin top-left); the maps' green channel points image-up (+Y, OpenGL /
// glTF convention), hence the minus.
static float3 PerturbNormal(float3 normal, float3 tangent, float handedness, float3 mapped)
{
    const float3 t = tangent - normal * dot(normal, tangent);
    if (dot(t, t) < 1.0e-12) {
        return normal;  // No usable tangent: keep the geometric normal.
    }
    const float3 unitTangent = normalize(t);
    const float3 bitangent = cross(normal, unitTangent) * handedness;
    // A zero texel (a broken or unset map) has no direction: keep the normal.
    return SafeNormalize(unitTangent * mapped.x - bitangent * mapped.y + normal * mapped.z,
                         normal);
}

// Returns the tangent-space normal stored in `texel` (sampled from the
// normal map), unpacked to [-1, 1].
static float3 UnpackNormal(thread const MaterialResources& mat, float4 texel)
{
    if ((mat.params->options.y & kMaterialFlagNormalRg) != 0u) {
        const float2 xy = texel.xy * 2.0 - 1.0;
        return float3(xy, sqrt(clamp(1.0 - dot(xy, xy), 0.0, 1.0)));
    }
    return texel.xyz * 2.0 - 1.0;
}

// Returns the light from the frame's point lights (unshadowed): inverse
// square falloff, faded to zero at the range (render::PointLight).
static float3 PointLights(thread const FrameResources& res, thread const Surface& s)
{
    constant FrameUniforms& frame = *res.frame;
    float3 color = float3(0.0);
    const uint count = min(frame.pointLightCount.x, uint(kMaxPointLights));
    for (uint i = 0u; i < count; ++i) {
        const GpuPointLight light = frame.pointLights[i];
        const float3 toLight = light.positionInverseRangeSq.xyz - s.position;
        const float distanceSq = max(dot(toLight, toLight), 1.0e-4);
        const float ratio = distanceSq * light.positionInverseRangeSq.w;
        const float window = clamp(1.0 - ratio * ratio, 0.0, 1.0);
        const float attenuation = window * window / distanceSq;
        if (attenuation > 0.0) {
            color += DirectLight(s, toLight * rsqrt(distanceSq),
                                 light.radiance.rgb * attenuation);
        }
    }
    return color;
}

fragment float4 FragmentMain(
    MeshVaryings in [[stage_in]],
    bool frontFacing [[front_facing]],
    constant FrameUniforms& frame [[buffer(kBufferFrame)]],
    depth2d_array<float> shadowMap [[texture(kTextureShadowMap)]],
    sampler shadowSampler [[sampler(kSamplerShadow)]],
    texturecube<float> environmentMap [[texture(kTextureEnvironment)]],
    texture2d<float> brdfLut [[texture(kTextureBrdfLut)]],
    texture2d<float> ambientOcclusionMap [[texture(kTextureAmbientOcclusion)]],
    sampler linearClamp [[sampler(kSamplerLinearClamp)]],
    constant MaterialParams& material [[buffer(kBufferMaterial)]],
    texture2d<float> baseColorMap [[texture(kTextureBaseColor)]],
    texture2d<float> metallicRoughnessMap [[texture(kTextureMetallicRoughness)]],
    texture2d<float> normalMap [[texture(kTextureNormal)]],
    texture2d<float> occlusionMap [[texture(kTextureOcclusion)]],
    texture2d<float> emissiveMap [[texture(kTextureEmissive)]],
    sampler materialSampler [[sampler(kSamplerMaterial)]])
{
    const FrameResources res = {&frame, shadowMap, shadowSampler, environmentMap,
                                brdfLut, ambientOcclusionMap, linearClamp};
    const MaterialResources mat = {&material, baseColorMap, metallicRoughnessMap,
                                   normalMap, occlusionMap, emissiveMap,
                                   materialSampler};

    // Every texture is sampled before the alpha test so implicit derivatives
    // stay defined in the whole quad.
    const float2 uv0 = in.uv.xy;
    const float2 uv1 = in.uv.zw;
    const float4 baseColor =
        mat.baseColorMap.sample(mat.materialSampler, MaterialUv(material, kSlotBaseColor, uv0, uv1)) *
        material.baseColor;
    const float4 metallicRoughness =
        mat.metallicRoughnessMap.sample(mat.materialSampler,
                                        MaterialUv(material, kSlotMetallicRoughness, uv0, uv1));
    float3 mapped = UnpackNormal(
        mat, mat.normalMap.sample(mat.materialSampler, MaterialUv(material, kSlotNormal, uv0, uv1)));
    const float occlusionTexel =
        (material.options.y & kMaterialFlagOcclusionInMetallicRoughness) != 0u
            ? metallicRoughness.r
            : mat.occlusionMap.sample(mat.materialSampler,
                                      MaterialUv(material, kSlotOcclusion, uv0, uv1)).r;
    const float3 emissive =
        mat.emissiveMap.sample(mat.materialSampler, MaterialUv(material, kSlotEmissive, uv0, uv1)).rgb *
        material.emissive.rgb;

    // Degenerate vertex normals (zero, or cancelling across the triangle)
    // fall back to facing the camera.
    float3 geometricNormal = SafeNormalize(
        in.normal, SafeNormalize(frame.cameraPosition.xyz - in.worldPosition, float3(0.0, 1.0, 0.0)));
    float3 tangent = in.tangent.xyz;
    if (kDoubleSided && !frontFacing) {
        // Back face: mirror the whole tangent frame. The bitangent comes from
        // cross(N, T), which is unchanged when both flip, so flip its sign too.
        geometricNormal = -geometricNormal;
        tangent = -tangent;
        mapped.y = -mapped.y;
    }
    mapped.xy *= material.surface.z;
    const float3 normal = PerturbNormal(geometricNormal, tangent, in.tangent.w, mapped);

    const float roughness = FilterRoughness(metallicRoughness.g * material.surface.y, normal);

    if (kAlphaMask && baseColor.a < material.uvAlpha.z) {
        discard_fragment();
    }

    const float metallic = clamp(metallicRoughness.b * material.surface.x, 0.0, 1.0);
    // Strengths above 1 would push it below 0.
    const float occlusion =
        clamp(1.0 + material.surface.w * (occlusionTexel - 1.0), 0.0, 1.0);
    const Surface s = MakeSurface(res, in.worldPosition, normal, baseColor.rgb, metallic,
                                  roughness, occlusion);

    float3 color = PointLights(res, s);

    // Sun. Surfaces whose geometry faces away from it get no direct light,
    // shadow lookup included (a bumped normal must not light the back side).
    // A scene without a sun may leave its direction zero (and its colour 0,
    // but 0 * NaN is still NaN).
    const float3 sunDir = -SafeNormalize(frame.sunDirection.xyz, float3(0.0, -1.0, 0.0));
    const float viewDepth = -(frame.view * float4(in.worldPosition, 1.0)).z;
    float visibility = dot(geometricNormal, sunDir) > 0.0 ? 1.0 : 0.0;
    if (frame.shadowParams.x > 0.5 && visibility > 0.0) {
        visibility = SunVisibility(res, in.worldPosition, geometricNormal, viewDepth);
    }
    color += DirectLight(s, sunDir, frame.sunColor.rgb * visibility);

    color += AmbientLight(res, s, geometricNormal, in.position.xy) + emissive;
    color = ApplyCascadeDebugTint(res, color, viewDepth);
    // Defence in depth: whatever slipped through above must not reach the
    // HDR target as NaN / Inf (or overflow its half floats), see
    // sanitize.h.
    return float4(SanitizeRadiance(color), 1.0);
}
