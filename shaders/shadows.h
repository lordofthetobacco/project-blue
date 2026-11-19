// Cascaded sun shadow lookups. Included with #include "shadows.h" after
// <metal_stdlib>, "bindings.h" and the using-directive; it includes common.h
// itself. Helpers take `thread const FrameResources&` (common.h).
//
// The one coordinate difference from the Vulkan version: Metal's texture
// origin is the top-left corner while light-space NDC y points up, so the
// shadow-map v is 0.5 - 0.5 * y (the shadow pass renders with a plain
// viewport and unchanged matrices).
#ifndef BLUE_SHADERS_SHADOWS_H_
#define BLUE_SHADERS_SHADOWS_H_

#include "common.h"

// Fraction of a cascade's depth range (and of the shadow distance) over which
// it blends into the next cascade (or fades out).
constant float kBlendFraction = 0.1;
constant float kDebugTintStrength = 0.35;
constant float3 kCascadeTints[kCascadeCount] = {
    float3(1.0, 0.1, 0.1), float3(0.1, 1.0, 0.1), float3(0.1, 0.3, 1.0), float3(1.0, 0.9, 0.1)};

// Returns the lit fraction (0 = shadowed, 1 = lit) of `worldPosition` in
// `cascade`: the position is moved along the normal by a few texels to avoid
// self-shadowing, then filtered with a 3x3 grid of hardware-filtered taps.
static inline float SampleCascade(thread const FrameResources& res, int cascade,
                                  float3 worldPosition, float3 normal)
{
    constant FrameUniforms& frame = *res.frame;
    const float offsetScale = frame.cascadeTexelWorldSize[cascade] * frame.shadowParams.w;
    const float4 lightClip = frame.cascadeViewProjection[cascade] * float4(worldPosition + normal * offsetScale, 1.0);
    const float3 coord = float3(lightClip.x * 0.5 + 0.5, 0.5 - 0.5 * lightClip.y, lightClip.z);

    const float2 texel = 1.0 / float2(res.shadowMap.get_width(), res.shadowMap.get_height());
    float lit = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            const float2 uv = coord.xy + float2(x, y) * texel;
            lit += res.shadowMap.sample_compare(res.shadowSampler, uv, uint(cascade), coord.z);
        }
    }
    return lit / 9.0;
}

// Returns the index of the first cascade that reaches `viewDepth`, or
// kCascadeCount beyond the shadow distance.
static inline int CascadeIndex(thread const FrameResources& res, float viewDepth)
{
    for (int i = 0; i < kCascadeCount; ++i) {
        if (res.frame->cascadeSplitFar[i] > viewDepth) {
            return i;
        }
    }
    return kCascadeCount;
}

// Returns the lit fraction of `worldPosition` at camera distance `viewDepth`.
static inline float SunVisibility(thread const FrameResources& res, float3 worldPosition,
                                  float3 normal, float viewDepth)
{
    constant FrameUniforms& frame = *res.frame;
    const int cascade = CascadeIndex(res, viewDepth);
    if (cascade == kCascadeCount) {
        return 1.0;
    }

    float visibility = SampleCascade(res, cascade, worldPosition, normal);

    const float cascadeFar = frame.cascadeSplitFar[cascade];
    const float cascadeNear = cascade == 0 ? 0.0 : frame.cascadeSplitFar[cascade - 1];
    // Empty ranges (equal splits, zero shadow distance) must not divide 0 / 0.
    const float blendRange = metal::max((cascadeFar - cascadeNear) * kBlendFraction, 1.0e-6);
    const float blend = metal::clamp((viewDepth - (cascadeFar - blendRange)) / blendRange, 0.0, 1.0);
    if (cascade + 1 < kCascadeCount && blend > 0.0) {
        visibility = metal::mix(visibility, SampleCascade(res, cascade + 1, worldPosition, normal), blend);
    }

    const float shadowDistance = frame.shadowParams.z;
    const float fadeRange = metal::max(shadowDistance * kBlendFraction, 1.0e-6);
    const float fade = metal::clamp((viewDepth - (shadowDistance - fadeRange)) / fadeRange, 0.0, 1.0);
    return metal::mix(visibility, 1.0, fade);
}

// Returns `color` tinted by the cascade that covers `viewDepth` when the
// cascade debug view is on.
static inline float3 ApplyCascadeDebugTint(thread const FrameResources& res, float3 color,
                                           float viewDepth)
{
    const int cascade = CascadeIndex(res, viewDepth);
    if (res.frame->shadowParams.x > 0.5 && res.frame->shadowParams.y > 0.5 &&
        cascade < kCascadeCount) {
        return metal::mix(color, kCascadeTints[cascade], float3(kDebugTintStrength));
    }
    return color;
}

#endif  // BLUE_SHADERS_SHADOWS_H_
