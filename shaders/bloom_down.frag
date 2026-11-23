#version 450

// One downsample step of the bloom chain (render::BloomPass): the 13-tap
// filter of Jimenez, "Next Generation Post Processing in Call of Duty:
// Advanced Warfare" (2014): five overlapping 2x2 box groups, the centre
// group weighted 0.5 and the four corner groups 0.125 each. The first step
// (from the HDR image) weights each group by 1 / (1 + luma) ("Karis
// average"), so a single extremely bright pixel cannot become a flickering
// blob.

#include "sanitize.glsl"

layout(set = 0, binding = 0) uniform sampler2D source;  // bilinear, clamped

// Must match DownsampleConstants in src/render/vulkan/passes/bloom_pass.cc.
layout(push_constant) uniform DownsampleConstants {
    vec2 sourceTexel;  // 1 / source size
    int karisAverage;  // 1 on the first step
} params;

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

// Every tap is sanitised: NaN / Inf from a broken pixel never spreads, and
// the radiance passed on stays at the scene maximum. (It was 32768 here,
// which the up-sample summing six levels could take past the fp16 maximum
// to +Inf, and the composite then to black.)
vec3 Tap(vec2 offset)
{
    return SanitizeRadiance(texture(source, inUv + offset * params.sourceTexel).rgb);
}

float KarisWeight(vec3 color)
{
    return 1.0 / (1.0 + dot(color, vec3(0.2126, 0.7152, 0.0722)));
}

void main()
{
    // a b c
    //  j k
    // d e f
    //  l m
    // g h i
    const vec3 a = Tap(vec2(-2.0, -2.0));
    const vec3 b = Tap(vec2(0.0, -2.0));
    const vec3 c = Tap(vec2(2.0, -2.0));
    const vec3 d = Tap(vec2(-2.0, 0.0));
    const vec3 e = Tap(vec2(0.0, 0.0));
    const vec3 f = Tap(vec2(2.0, 0.0));
    const vec3 g = Tap(vec2(-2.0, 2.0));
    const vec3 h = Tap(vec2(0.0, 2.0));
    const vec3 i = Tap(vec2(2.0, 2.0));
    const vec3 j = Tap(vec2(-1.0, -1.0));
    const vec3 k = Tap(vec2(1.0, -1.0));
    const vec3 l = Tap(vec2(-1.0, 1.0));
    const vec3 m = Tap(vec2(1.0, 1.0));

    const vec3 groups[5] = vec3[](
        (j + k + l + m) * 0.25,
        (a + b + d + e) * 0.25,
        (b + c + e + f) * 0.25,
        (d + e + g + h) * 0.25,
        (e + f + h + i) * 0.25);
    const float baseWeights[5] = float[](0.5, 0.125, 0.125, 0.125, 0.125);

    vec3 sum = vec3(0.0);
    float weightSum = 0.0;
    for (int n = 0; n < 5; ++n) {
        const float weight =
            baseWeights[n] * (params.karisAverage != 0 ? KarisWeight(groups[n]) : 1.0);
        sum += groups[n] * weight;
        weightSum += weight;
    }
    outColor = vec4(SanitizeRadiance(sum / weightSum), 1.0);
}
