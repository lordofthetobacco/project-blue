// One upsample step of the bloom chain (render BloomPass): a 3x3 tent
// filter (weights 1 2 1 / 2 4 2 / 1 2 1, divided by 16) of the smaller mip,
// added (blend ONE, ONE) onto the next larger one, which still holds its
// downsampled image. Mip 0 ends up with the sum of every level.
// Program: bloom_up.

#include "fullscreen.slang"
#include "sanitize.slang"

BLUE_TEXTURE(Texture2D<float4>, source, 0, BLUE_INPUT_SET);  // bilinear, clamped

// Must match UpsampleConstants in src/render/vulkan/passes/bloom_pass.cc.
struct UpsampleConstants {
    float2 sourceTexel;  // 1 / source size: the tent's tap spacing
};
BLUE_PUSH_CONSTANTS(UpsampleConstants, params);

float3 Tap(float2 uv, float x, float y)
{
    // A non-finite texel contributes nothing (see sanitize.slang).
    const float2 tapUv = uv + float2(x, y) * params.sourceTexel;
    return SanitizeRadiance(source.Sample(inputSampler, tapUv).rgb);
}

[shader("fragment")]
float4 FragmentMain(FullscreenVaryings input) : SV_Target
{
    const float2 uv = input.uv;
    float3 sum = Tap(uv, 0.0, 0.0) * 4.0;
    sum += (Tap(uv, -1.0, 0.0) + Tap(uv, 1.0, 0.0) + Tap(uv, 0.0, -1.0) + Tap(uv, 0.0, 1.0)) * 2.0;
    sum += Tap(uv, -1.0, -1.0) + Tap(uv, 1.0, -1.0) + Tap(uv, -1.0, 1.0) + Tap(uv, 1.0, 1.0);
    return float4(sum / 16.0, 1.0);
}
