#include <metal_stdlib>

#include "bindings.h"

using namespace metal;

#include "environment.h"
#include "sanitize.h"

// GGX-prefilters one face of one mip of the specular environment cube
// (render::Environment): the split-sum prefiltered radiance with N = V = R
// (Karis 2013). Each sample reads the source mip whose texel solid angle
// matches the sample's (filtered importance sampling, Krivanek and Colbert
// 2008), which removes fireflies with few samples.

// Must match PrefilterConstants in src/render/environment.mm.
struct PrefilterConstants {
    int face;
    float roughness;    // perceptual
    float sourceSize;   // texels per face of source mip 0
    int sampleCount;
};

// Must match FullscreenVaryings in fullscreen.vert.metal.
struct FullscreenVaryings {
    float4 position [[position]];
    float2 uv [[user(loc0)]];
};

static float DistributionGgx(float NdotH, float alpha)
{
    const float a2 = alpha * alpha;
    const float d = (NdotH * a2 - NdotH) * NdotH + 1.0;
    return a2 / (kEnvPi * d * d);
}

fragment float4 FragmentMain(FullscreenVaryings varyings [[stage_in]],
                             texturecube<float> source [[texture(kTextureInput0)]],
                             sampler sourceSampler [[sampler(kSamplerInput)]],
                             constant PrefilterConstants& pass [[buffer(kBufferPass)]])
{
    const float3 n = CubeFaceDirection(pass.face, varyings.uv);
    if (pass.sampleCount <= 1 || pass.roughness <= 0.0) {
        return float4(SanitizeColor(source.sample(sourceSampler, n, level(0.0)).rgb, kMaxHalfRadiance), 1.0);
    }
    const float alpha = pass.roughness * pass.roughness;
    const float3x3 basis = TangentFrame(n);
    const float texelSolidAngle = 4.0 * kEnvPi / (6.0 * pass.sourceSize * pass.sourceSize);
    const uint count = uint(pass.sampleCount);

    float3 sum = float3(0.0);
    float weight = 0.0;
    for (uint i = 0u; i < count; ++i) {
        const float3 h = basis * ImportanceSampleGgx(Hammersley(i, count), alpha);
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
        sum += source.sample(sourceSampler, l, level(lod)).rgb * NdotL;
        weight += NdotL;
    }
    return float4(SanitizeColor(sum / max(weight, 1.0e-6), kMaxHalfRadiance), 1.0);
}
