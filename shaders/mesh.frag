#version 450

#include "common.glsl"
#include "shadows.glsl"
#include "pbr.glsl"
#include "material.glsl"

// Pipeline variants (render::MaterialVariant).
layout(constant_id = 0) const bool kAlphaMask = false;
layout(constant_id = 1) const bool kDoubleSided = false;

layout(location = 0) in vec3 inWorldPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inUv;  // xy: uv0, zw: uv1
layout(location = 3) in vec4 inTangent;

layout(location = 0) out vec4 outColor;

// Returns the shading normal: `normal` perturbed by the tangent-space normal
// `mapped` (already unpacked to [-1, 1]). The mesh's bitangent
// cross(N, T) * w points towards increasing v, which is image-down (UV
// origin top-left); the maps' green channel points image-up (+Y, OpenGL /
// glTF convention), hence the minus.
vec3 PerturbNormal(vec3 normal, vec3 tangent, float handedness, vec3 mapped)
{
    const vec3 t = tangent - normal * dot(normal, tangent);
    if (dot(t, t) < 1.0e-12) {
        return normal;  // No usable tangent: keep the geometric normal.
    }
    const vec3 unitTangent = normalize(t);
    const vec3 bitangent = cross(normal, unitTangent) * handedness;
    // A zero texel (a broken or unset map) has no direction: keep the normal.
    return SafeNormalize(unitTangent * mapped.x - bitangent * mapped.y + normal * mapped.z,
                         normal);
}

// Returns the tangent-space normal stored in `texel` (sampled from the
// normal map), unpacked to [-1, 1].
vec3 UnpackNormal(vec4 texel)
{
    if ((material.options.y & kMaterialFlagNormalRg) != 0u) {
        const vec2 xy = texel.xy * 2.0 - 1.0;
        return vec3(xy, sqrt(clamp(1.0 - dot(xy, xy), 0.0, 1.0)));
    }
    return texel.xyz * 2.0 - 1.0;
}

// Returns the light from the frame's point lights (unshadowed): inverse
// square falloff, faded to zero at the range (render::PointLight).
vec3 PointLights(Surface s)
{
    vec3 color = vec3(0.0);
    const uint count = min(frame.pointLightCount.x, uint(kMaxPointLights));
    for (uint i = 0u; i < count; ++i) {
        const GpuPointLight light = frame.pointLights[i];
        const vec3 toLight = light.positionInverseRangeSq.xyz - s.position;
        const float distanceSq = max(dot(toLight, toLight), 1.0e-4);
        const float ratio = distanceSq * light.positionInverseRangeSq.w;
        const float window = clamp(1.0 - ratio * ratio, 0.0, 1.0);
        const float attenuation = window * window / distanceSq;
        if (attenuation > 0.0) {
            color += DirectLight(s, toLight * inversesqrt(distanceSq),
                                 light.radiance.rgb * attenuation);
        }
    }
    return color;
}

void main()
{
    // Every texture is sampled before the alpha test so implicit derivatives
    // stay defined in the whole quad.
    const vec2 uv0 = inUv.xy;
    const vec2 uv1 = inUv.zw;
    const vec4 baseColor =
        texture(baseColorMap, MaterialUv(kSlotBaseColor, uv0, uv1)) * material.baseColor;
    const vec4 metallicRoughness =
        texture(metallicRoughnessMap, MaterialUv(kSlotMetallicRoughness, uv0, uv1));
    vec3 mapped = UnpackNormal(texture(normalMap, MaterialUv(kSlotNormal, uv0, uv1)));
    const float occlusionTexel =
        (material.options.y & kMaterialFlagOcclusionInMetallicRoughness) != 0u
            ? metallicRoughness.r
            : texture(occlusionMap, MaterialUv(kSlotOcclusion, uv0, uv1)).r;
    const vec3 emissive =
        texture(emissiveMap, MaterialUv(kSlotEmissive, uv0, uv1)).rgb * material.emissive.rgb;

    // Degenerate vertex normals (zero, or cancelling across the triangle)
    // fall back to facing the camera.
    vec3 geometricNormal = SafeNormalize(
        inNormal, SafeNormalize(frame.cameraPosition.xyz - inWorldPosition, vec3(0.0, 1.0, 0.0)));
    vec3 tangent = inTangent.xyz;
    if (kDoubleSided && !gl_FrontFacing) {
        // Back face: mirror the whole tangent frame. The bitangent comes from
        // cross(N, T), which is unchanged when both flip, so flip its sign too.
        geometricNormal = -geometricNormal;
        tangent = -tangent;
        mapped.y = -mapped.y;
    }
    mapped.xy *= material.surface.z;
    const vec3 normal = PerturbNormal(geometricNormal, tangent, inTangent.w, mapped);

    const float roughness = FilterRoughness(metallicRoughness.g * material.surface.y, normal);

    if (kAlphaMask && baseColor.a < material.uvAlpha.z) {
        discard;
    }

    const float metallic = clamp(metallicRoughness.b * material.surface.x, 0.0, 1.0);
    // Strengths above 1 would push it below 0.
    const float occlusion =
        clamp(1.0 + material.surface.w * (occlusionTexel - 1.0), 0.0, 1.0);
    const Surface s = MakeSurface(inWorldPosition, normal, baseColor.rgb, metallic,
                                  roughness, occlusion);

    vec3 color = PointLights(s);

    // Sun. Surfaces whose geometry faces away from it get no direct light,
    // shadow lookup included (a bumped normal must not light the back side).
    // A scene without a sun may leave its direction zero (and its colour 0,
    // but 0 * NaN is still NaN).
    const vec3 sunDir = -SafeNormalize(frame.sunDirection.xyz, vec3(0.0, -1.0, 0.0));
    const float viewDepth = -(frame.view * vec4(inWorldPosition, 1.0)).z;
    float visibility = dot(geometricNormal, sunDir) > 0.0 ? 1.0 : 0.0;
    if (frame.shadowParams.x > 0.5 && visibility > 0.0) {
        visibility = SunVisibility(inWorldPosition, geometricNormal, viewDepth);
    }
    color += DirectLight(s, sunDir, frame.sunColor.rgb * visibility);

    color += AmbientLight(s, geometricNormal) + emissive;
    color = ApplyCascadeDebugTint(color, viewDepth);
    // Defence in depth: whatever slipped through above must not reach the
    // HDR target as NaN / Inf (or overflow its half floats), see
    // sanitize.glsl.
    outColor = vec4(SanitizeRadiance(color), 1.0);
}
