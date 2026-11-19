#include <metal_stdlib>

#include "bindings.h"

using namespace metal;

#include "sanitize.h"

// One upsample step of the bloom chain (render::BloomPass): a 3x3 tent
// filter (weights 1 2 1 / 2 4 2 / 1 2 1, divided by 16) of the smaller mip,
// added (blend One, One) onto the next larger one, which still holds its
// downsampled image. Mip 0 ends up with the sum of every level.

// Must match UpsampleConstants in src/render/passes/bloom_pass.mm.
struct UpsampleConstants {
    float2 sourceTexel;  // 1 / source size: the tent's tap spacing
};

// Must match FullscreenVaryings in fullscreen.vert.metal.
struct FullscreenVaryings {
    float4 position [[position]];
    float2 uv [[user(loc0)]];
};

// The pass's inputs, gathered so that Tap() takes one argument.
struct UpsampleInputs {
    texture2d<float> source;  // bilinear, clamped
    sampler sourceSampler;
    float2 uv;
    float2 sourceTexel;
};

static float3 Tap(thread const UpsampleInputs& inputs, float x, float y)
{
    // A non-finite texel contributes nothing (see sanitize.h).
    return SanitizeRadiance(inputs.source.sample(inputs.sourceSampler, inputs.uv + float2(x, y) * inputs.sourceTexel).rgb);
}

fragment float4 FragmentMain(FullscreenVaryings varyings [[stage_in]],
                             texture2d<float> source [[texture(kTextureInput0)]],
                             sampler sourceSampler [[sampler(kSamplerInput)]],
                             constant UpsampleConstants& params [[buffer(kBufferPass)]])
{
    const UpsampleInputs inputs = {source, sourceSampler, varyings.uv, params.sourceTexel};
    float3 sum = Tap(inputs, 0.0, 0.0) * 4.0;
    sum += (Tap(inputs, -1.0, 0.0) + Tap(inputs, 1.0, 0.0) + Tap(inputs, 0.0, -1.0) + Tap(inputs, 0.0, 1.0)) * 2.0;
    sum += Tap(inputs, -1.0, -1.0) + Tap(inputs, 1.0, -1.0) + Tap(inputs, -1.0, 1.0) + Tap(inputs, 1.0, 1.0);
    return float4(sum / 16.0, 1.0);
}
