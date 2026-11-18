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
layout(location = 2) in vec2 inUv;
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
    return normalize(unitTangent * mapped.x - bitangent * mapped.y + normal * mapped.z);
}

void main()
{
    // Every texture is sampled before the alpha test so implicit derivatives
    // stay defined in the whole quad.
    const vec2 uv = inUv * material.uvAlpha.xy;
    const vec4 baseColor = texture(baseColorMap, uv) * material.baseColor;
    const vec4 metallicRoughness = texture(metallicRoughnessMap, uv);
    vec3 mapped = texture(normalMap, uv).xyz * 2.0 - 1.0;
    const float occlusionTexel = texture(occlusionMap, uv).r;
    const vec3 emissive = texture(emissiveMap, uv).rgb * material.emissive.rgb;

    vec3 geometricNormal = normalize(inNormal);
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
    const float occlusion = 1.0 + material.surface.w * (occlusionTexel - 1.0);
    const Surface s = MakeSurface(inWorldPosition, normal, baseColor.rgb, metallic,
                                  roughness, occlusion);

    // Point fill light (unshadowed), inverse-square falloff.
    const vec3 toLight = frame.lightPosition.xyz - inWorldPosition;
    const float distanceSq = max(dot(toLight, toLight), 1.0e-4);
    vec3 color = DirectLight(s, toLight * inversesqrt(distanceSq),
                             frame.lightColor.rgb * frame.lightColor.a / distanceSq);

    // Sun. Surfaces whose geometry faces away from it get no direct light,
    // shadow lookup included (a bumped normal must not light the back side).
    const vec3 sunDir = -normalize(frame.sunDirection.xyz);
    const float viewDepth = -(frame.view * vec4(inWorldPosition, 1.0)).z;
    float visibility = dot(geometricNormal, sunDir) > 0.0 ? 1.0 : 0.0;
    if (frame.shadowParams.x > 0.5 && visibility > 0.0) {
        visibility = SunVisibility(inWorldPosition, geometricNormal, viewDepth);
    }
    color += DirectLight(s, sunDir, frame.sunColor.rgb * visibility);

    color += AmbientLight(s, geometricNormal) + emissive;
    color = ApplyCascadeDebugTint(color, viewDepth);
    outColor = vec4(color, 1.0);
}
