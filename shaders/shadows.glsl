// Cascaded sun shadow lookups. Included with #include "shadows.glsl" after
// common.glsl.

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

// Returns `color` tinted by the cascade that covers `viewDepth` when the
// cascade debug view is on.
vec3 ApplyCascadeDebugTint(vec3 color, float viewDepth)
{
    const int cascade = CascadeIndex(viewDepth);
    if (frame.shadowParams.x > 0.5 && frame.shadowParams.y > 0.5 && cascade < kCascadeCount) {
        return mix(color, kCascadeTints[cascade], kDebugTintStrength);
    }
    return color;
}
