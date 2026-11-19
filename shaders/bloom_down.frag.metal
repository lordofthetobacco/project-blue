#include <metal_stdlib>

#include "bindings.h"

using namespace metal;

#include "sanitize.h"

// One downsample step of the bloom chain (render::BloomPass): the 13-tap
// filter of Jimenez, "Next Generation Post Processing in Call of Duty:
// Advanced Warfare" (2014): five overlapping 2x2 box groups, the centre
// group weighted 0.5 and the four corner groups 0.125 each. The first step
// (from the HDR image) weights each group by 1 / (1 + luma) ("Karis
// average"), so a single extremely bright pixel cannot become a flickering
// blob.

// Must match DownsampleConstants in src/render/passes/bloom_pass.mm (16
// bytes: the float2 alignment pads the block).
struct DownsampleConstants {
    float2 sourceTexel;  // 1 / source size
    int karisAverage;    // 1 on the first step
};

// Must match FullscreenVaryings in fullscreen.vert.metal.
struct FullscreenVaryings {
    float4 position [[position]];
    float2 uv [[user(loc0)]];
};

// The pass's inputs, gathered so that Tap() takes one argument.
struct DownsampleInputs {
    texture2d<float> source;  // bilinear, clamped
    sampler sourceSampler;
    float2 uv;
    float2 sourceTexel;
};

// Every tap is sanitised: NaN / Inf from a broken pixel never spreads, and
// the radiance passed on stays at the scene maximum. (It was 32768 here,
// which the up-sample summing six levels could take past the fp16 maximum
// to +Inf, and the composite then to black.)
static float3 Tap(thread const DownsampleInputs& inputs, float2 offset)
{
    return SanitizeRadiance(inputs.source.sample(inputs.sourceSampler, inputs.uv + offset * inputs.sourceTexel).rgb);
}

static float KarisWeight(float3 color)
{
    return 1.0 / (1.0 + dot(color, float3(0.2126, 0.7152, 0.0722)));
}

fragment float4 FragmentMain(FullscreenVaryings varyings [[stage_in]],
                             texture2d<float> source [[texture(kTextureInput0)]],
                             sampler sourceSampler [[sampler(kSamplerInput)]],
                             constant DownsampleConstants& params [[buffer(kBufferPass)]])
{
    const DownsampleInputs inputs = {source, sourceSampler, varyings.uv, params.sourceTexel};

    // a b c
    //  j k
    // d e f
    //  l m
    // g h i
    const float3 a = Tap(inputs, float2(-2.0, -2.0));
    const float3 b = Tap(inputs, float2(0.0, -2.0));
    const float3 c = Tap(inputs, float2(2.0, -2.0));
    const float3 d = Tap(inputs, float2(-2.0, 0.0));
    const float3 e = Tap(inputs, float2(0.0, 0.0));
    const float3 f = Tap(inputs, float2(2.0, 0.0));
    const float3 g = Tap(inputs, float2(-2.0, 2.0));
    const float3 h = Tap(inputs, float2(0.0, 2.0));
    const float3 i = Tap(inputs, float2(2.0, 2.0));
    const float3 j = Tap(inputs, float2(-1.0, -1.0));
    const float3 k = Tap(inputs, float2(1.0, -1.0));
    const float3 l = Tap(inputs, float2(-1.0, 1.0));
    const float3 m = Tap(inputs, float2(1.0, 1.0));

    const float3 groups[5] = {
        (j + k + l + m) * 0.25,
        (a + b + d + e) * 0.25,
        (b + c + e + f) * 0.25,
        (d + e + g + h) * 0.25,
        (e + f + h + i) * 0.25};
    const float baseWeights[5] = {0.5, 0.125, 0.125, 0.125, 0.125};

    float3 sum = float3(0.0);
    float weightSum = 0.0;
    for (int n = 0; n < 5; ++n) {
        const float weight =
            baseWeights[n] * (params.karisAverage != 0 ? KarisWeight(groups[n]) : 1.0);
        sum += groups[n] * weight;
        weightSum += weight;
    }
    return float4(SanitizeRadiance(sum / weightSum), 1.0);
}
