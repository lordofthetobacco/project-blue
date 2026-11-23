// GGX-prefilters one face of one mip of the specular environment cube
// (render Environment): the split-sum prefiltered radiance with N = V = R
// (Karis 2013). Each sample reads the source mip whose texel solid angle
// matches the sample's (filtered importance sampling, Krivanek and Colbert
// 2008), which removes fireflies with few samples. Program: env_prefilter.

#include "environment.slang"
#include "fullscreen.slang"
#include "sanitize.slang"

BLUE_TEXTURE(TextureCube<float4>, source, 0, BLUE_INPUT_SET);

// Must match PrefilterConstants in src/render/vulkan/environment.cc.
struct PrefilterConstants {
    int face;
    float roughness;   // perceptual
    float sourceSize;  // texels per face of source mip 0
    int sampleCount;
};
BLUE_PUSH_CONSTANTS(PrefilterConstants, push);

float DistributionGgx(float NdotH, float alpha)
{
    const float a2 = alpha * alpha;
    const float d = (NdotH * a2 - NdotH) * NdotH + 1.0;
    return a2 / (kEnvPi * d * d);
}

[shader("fragment")]
float4 FragmentMain(FullscreenVaryings input) : SV_Target
{
    const float3 n = CubeFaceDirection(push.face, input.uv);
    if (push.sampleCount <= 1 || push.roughness <= 0.0) {
        const float3 radiance = source.SampleLevel(inputSampler, n, 0.0).rgb;
        return float4(SanitizeColor(radiance, kMaxHalfRadiance), 1.0);
    }
    const float alpha = push.roughness * push.roughness;
    const float texelSolidAngle = 4.0 * kEnvPi / (6.0 * push.sourceSize * push.sourceSize);
    const uint count = uint(push.sampleCount);

    float3 sum = float3(0.0);
    float weight = 0.0;
    for (uint i = 0u; i < count; ++i) {
        const float3 h = FromTangentFrame(n, ImportanceSampleGgx(Hammersley(i, count), alpha));
        const float NdotH = dot(n, h);
        const float3 l = 2.0 * NdotH * h - n;  // V = N
        const float NdotL = dot(n, l);
        if (NdotL <= 0.0) {
            continue;
        }
        // pdf of l = D * NdotH / (4 VdotH) = D / 4 with V = N.
        const float pdf = DistributionGgx(max(NdotH, 0.0), alpha) * 0.25;
        const float sampleSolidAngle = 1.0 / (float(count) * pdf + 1.0e-6);
        const float lod = max(0.5 * log2(sampleSolidAngle / texelSolidAngle) + 1.0, 0.0);
        sum += source.SampleLevel(inputSampler, l, lod).rgb * NdotL;
        weight += NdotL;
    }
    return float4(SanitizeColor(sum / max(weight, 1.0e-6), kMaxHalfRadiance), 1.0);
}
