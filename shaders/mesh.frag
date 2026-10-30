#version 450

#include "common.glsl"

layout(location = 0) in vec3 inWorldPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inColor;

layout(location = 0) out vec4 outColor;

const float kShininess = 48.0;
const float kSpecularStrength = 0.35;
// Fraction of a cascade's depth range (and of the shadow distance) over which
// it blends into the next cascade (or fades out).
const float kBlendFraction = 0.1;
const float kDebugTintStrength = 0.35;
const vec3 kCascadeTints[kCascadeCount] = vec3[](
    vec3(1.0, 0.1, 0.1), vec3(0.1, 1.0, 0.1), vec3(0.1, 0.3, 1.0), vec3(1.0, 0.9, 0.1));

// Returns the lit fraction (0 = shadowed, 1 = lit) of `worldPosition` in
// `cascade`: the position is moved along the normal by a few texels to avoid
// self-shadowing, then filtered with a 3x3 grid of hardware-filtered taps.
float SampleCascade(int cascade, vec3 worldPosition, vec3 normal)
{
    const float offsetScale = frame.cascadeTexelWorldSize[cascade] * frame.shadowParams.w;
    const vec4 lightClip = frame.cascadeViewProjection[cascade] * vec4(worldPosition + normal * offsetScale, 1.0);
    const vec3 coord = vec3(lightClip.xy * 0.5 + 0.5, lightClip.z);

    const vec2 texel = 1.0 / vec2(textureSize(shadowMap, 0).xy);
    float lit = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            const vec2 uv = coord.xy + vec2(x, y) * texel;
            lit += texture(shadowMap, vec4(uv, float(cascade), coord.z));
        }
    }
    return lit / 9.0;
}

// Returns the index of the first cascade that reaches `viewDepth`, or
// kCascadeCount beyond the shadow distance.
int CascadeIndex(float viewDepth)
{
    for (int i = 0; i < kCascadeCount; ++i) {
        if (frame.cascadeSplitFar[i] > viewDepth) {
            return i;
        }
    }
    return kCascadeCount;
}

// Returns the lit fraction of `worldPosition` at camera distance `viewDepth`.
float SunVisibility(vec3 worldPosition, vec3 normal, float viewDepth)
{
    const int cascade = CascadeIndex(viewDepth);
    if (cascade == kCascadeCount) {
        return 1.0;
    }

    float visibility = SampleCascade(cascade, worldPosition, normal);

    const float cascadeFar = frame.cascadeSplitFar[cascade];
    const float cascadeNear = cascade == 0 ? 0.0 : frame.cascadeSplitFar[cascade - 1];
    const float blendRange = (cascadeFar - cascadeNear) * kBlendFraction;
    const float blend = clamp((viewDepth - (cascadeFar - blendRange)) / blendRange, 0.0, 1.0);
    if (cascade + 1 < kCascadeCount && blend > 0.0) {
        visibility = mix(visibility, SampleCascade(cascade + 1, worldPosition, normal), blend);
    }

    const float shadowDistance = frame.shadowParams.z;
    const float fadeRange = shadowDistance * kBlendFraction;
    const float fade = clamp((viewDepth - (shadowDistance - fadeRange)) / fadeRange, 0.0, 1.0);
    return mix(visibility, 1.0, fade);
}

// Blinn-Phong lighting from the shadowed sun and a point fill light,
// evaluated in world space, plus ambient.
void main()
{
    const vec3 normal = normalize(inNormal);
    const vec3 viewDir = normalize(frame.cameraPosition.xyz - inWorldPosition);

    // Point fill light (unshadowed).
    const vec3 toLight = frame.lightPosition.xyz - inWorldPosition;
    const vec3 lightDir = normalize(toLight);
    const vec3 halfDir = normalize(lightDir + viewDir);
    const float distanceSq = dot(toLight, toLight);
    const vec3 radiance = frame.lightColor.rgb * frame.lightColor.a / distanceSq;
    const float diffuse = max(dot(normal, lightDir), 0.0);
    const float specular = diffuse > 0.0 ? pow(max(dot(normal, halfDir), 0.0), kShininess) : 0.0;

    // Sun. Surfaces facing away from it get no direct light, shadow lookup
    // included.
    const vec3 sunDir = -normalize(frame.sunDirection.xyz);
    const vec3 sunHalf = normalize(sunDir + viewDir);
    const float sunDiffuse = max(dot(normal, sunDir), 0.0);
    const bool shadowsOn = frame.shadowParams.x > 0.5;
    const float viewDepth = -(frame.view * vec4(inWorldPosition, 1.0)).z;
    float visibility = 1.0;
    if (shadowsOn && sunDiffuse > 0.0) {
        visibility = SunVisibility(inWorldPosition, normal, viewDepth);
    }
    const float sunSpecular = sunDiffuse > 0.0 ? pow(max(dot(normal, sunHalf), 0.0), kShininess) : 0.0;

    vec3 color = frame.ambientColor.rgb * inColor
               + radiance * (diffuse * inColor + specular * kSpecularStrength)
               + frame.sunColor.rgb * visibility * (sunDiffuse * inColor + sunSpecular * kSpecularStrength);

    const int cascade = CascadeIndex(viewDepth);
    if (shadowsOn && frame.shadowParams.y > 0.5 && cascade < kCascadeCount) {
        color = mix(color, kCascadeTints[cascade], kDebugTintStrength);
    }

    outColor = vec4(color, 1.0);
}
