// The forward PBR mesh pass (render MeshPass). Programs: mesh, mesh_masked,
// mesh_double, mesh_masked_double (defines ALPHA_MASK, DOUBLE_SIDED).

#include "common.slang"
#include "material.slang"
#include "pbr.slang"
#include "shadows.slang"

// Pipeline variants (render::MaterialVariant), one compiled program each.
static const bool kAlphaMask = ALPHA_MASK != 0;
static const bool kDoubleSided = DOUBLE_SIDED != 0;

// Must match DrawConstants in src/render/vulkan/draw_record.cc.
struct DrawConstants {
    float4x4 model;
};
BLUE_PUSH_CONSTANTS(DrawConstants, draw);

struct VertexInput {
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 normal : NORMAL;
    [[vk::location(2)]] float2 uv0 : TEXCOORD0;
    [[vk::location(3)]] float4 tangent : TANGENT;  // xyz: +u0 direction, w: handedness
    [[vk::location(4)]] float2 uv1 : TEXCOORD1;
};

struct Varyings {
    // Tested EQUAL against the depth pre-pass (depth.slang): both call
    // ClipPosition().
    float4 clipPosition : SV_Position;
    float3 worldPosition : WORLDPOSITION;
    float3 normal : NORMAL;
    float4 uv : TEXCOORD0;  // xy: uv0, zw: uv1
    float4 tangent : TANGENT;
};

[shader("vertex")]
Varyings VertexMain(VertexInput input)
{
    Varyings output;
    float4 worldPosition;
    output.clipPosition = ClipPosition(draw.model, input.position, worldPosition);

    // Normals transform by the inverse transpose of the model's 3x3, which is
    // its cofactor matrix divided by the determinant. The cofactor costs
    // three cross products; only the determinant's sign matters since the
    // normal is renormalized, and it also flips the handedness of mirroring
    // transforms. Tangents are surface directions: they take the plain 3x3.
    // (m[i] is row i; the cofactor's rows are the cross products of m's.)
    const float3x3 m = (float3x3)draw.model;
    const float3x3 cofactor =
        float3x3(cross(m[1], m[2]), cross(m[2], m[0]), cross(m[0], m[1]));
    const float mirror = dot(m[0], cofactor[0]) < 0.0 ? -1.0 : 1.0;

    output.worldPosition = worldPosition.xyz;
    output.normal = mul(cofactor, input.normal) * mirror;
    output.uv = float4(input.uv0, input.uv1);
    output.tangent = float4(mul(m, input.tangent.xyz), input.tangent.w * mirror);
    return output;
}

// Returns the shading normal: `normal` perturbed by the tangent-space normal
// `mapped` (already unpacked to [-1, 1]). The mesh's bitangent
// cross(N, T) * w points towards increasing v, which is image-down (UV
// origin top-left); the maps' green channel points image-up (+Y, OpenGL /
// glTF convention), hence the minus.
float3 PerturbNormal(float3 normal, float3 tangent, float handedness, float3 mapped)
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
float3 UnpackNormal(float4 texel)
{
    if ((material.options.y & kMaterialFlagNormalRg) != 0u) {
        const float2 xy = texel.xy * 2.0 - 1.0;
        return float3(xy, sqrt(clamp(1.0 - dot(xy, xy), 0.0, 1.0)));
    }
    return texel.xyz * 2.0 - 1.0;
}

// Returns the light from the frame's point lights (unshadowed): inverse
// square falloff, faded to zero at the range (render::PointLight).
float3 PointLights(Surface s)
{
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

[shader("fragment")]
float4 FragmentMain(Varyings input, bool isFrontFace : SV_IsFrontFace) : SV_Target
{
    // Every texture is sampled before the alpha test so implicit derivatives
    // stay defined in the whole quad.
    const float2 uv0 = input.uv.xy;
    const float2 uv1 = input.uv.zw;
    const float4 baseColor =
        baseColorMap.Sample(materialSampler, MaterialUv(kSlotBaseColor, uv0, uv1)) *
        material.baseColor;
    const float4 metallicRoughness =
        metallicRoughnessMap.Sample(materialSampler, MaterialUv(kSlotMetallicRoughness, uv0, uv1));
    float3 mapped =
        UnpackNormal(normalMap.Sample(materialSampler, MaterialUv(kSlotNormal, uv0, uv1)));
    const float occlusionSample =
        occlusionMap.Sample(materialSampler, MaterialUv(kSlotOcclusion, uv0, uv1)).r;
    const float occlusionTexel =
        (material.options.y & kMaterialFlagOcclusionInMetallicRoughness) != 0u
            ? metallicRoughness.r
            : occlusionSample;
    const float3 emissive =
        emissiveMap.Sample(materialSampler, MaterialUv(kSlotEmissive, uv0, uv1)).rgb *
        material.emissive.rgb;

    // Degenerate vertex normals (zero, or cancelling across the triangle)
    // fall back to facing the camera.
    float3 geometricNormal = SafeNormalize(
        input.normal,
        SafeNormalize(frame.cameraPosition.xyz - input.worldPosition, float3(0.0, 1.0, 0.0)));
    float3 tangent = input.tangent.xyz;
    if (kDoubleSided && !isFrontFace) {
        // Back face: mirror the whole tangent frame. The bitangent comes from
        // cross(N, T), which is unchanged when both flip, so flip its sign too.
        geometricNormal = -geometricNormal;
        tangent = -tangent;
        mapped.y = -mapped.y;
    }
    mapped.xy *= material.surface.z;
    const float3 normal = PerturbNormal(geometricNormal, tangent, input.tangent.w, mapped);

    const float roughness = FilterRoughness(metallicRoughness.g * material.surface.y, normal);

    if (kAlphaMask && baseColor.a < material.uvAlpha.z) {
        discard;
    }

    const float metallic = clamp(metallicRoughness.b * material.surface.x, 0.0, 1.0);
    // Strengths above 1 would push it below 0.
    const float occlusion =
        clamp(1.0 + material.surface.w * (occlusionTexel - 1.0), 0.0, 1.0);
    const Surface s = MakeSurface(input.worldPosition, normal, baseColor.rgb, metallic,
                                  roughness, occlusion);

    float3 color = PointLights(s);

    // Sun. Surfaces whose geometry faces away from it get no direct light,
    // shadow lookup included (a bumped normal must not light the back side).
    // A scene without a sun may leave its direction zero (and its colour 0,
    // but 0 * NaN is still NaN).
    const float3 sunDir = -SafeNormalize(frame.sunDirection.xyz, float3(0.0, -1.0, 0.0));
    const float viewDepth = -mul(frame.view, float4(input.worldPosition, 1.0)).z;
    float visibility = dot(geometricNormal, sunDir) > 0.0 ? 1.0 : 0.0;
    if (frame.shadowParams.x > 0.5 && visibility > 0.0) {
        visibility = SunVisibility(input.worldPosition, geometricNormal, viewDepth);
    }
    color += DirectLight(s, sunDir, frame.sunColor.rgb * visibility);

    color += AmbientLight(s, geometricNormal, input.clipPosition.xy) + emissive;
    color = ApplyCascadeDebugTint(color, viewDepth);
    // Defence in depth: whatever slipped through above must not reach the
    // HDR target as NaN / Inf (or overflow its half floats), see
    // sanitize.slang.
    return float4(SanitizeRadiance(color), 1.0);
}
