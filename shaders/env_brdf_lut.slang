// Generates the split-sum BRDF lookup table (render Environment), the GPU
// twin of lighting::GenerateBrdfLut(): x = NdotV, y = perceptual roughness
// (row 0 smoothest), rg = (scale, bias) so that the specular IBL term is
// prefiltered * (F0 * scale + F90 * bias). Importance-sampled GGX with
// height-correlated Smith visibility. Program: env_brdf_lut.
//
// The pass binds one input like every full-screen pass; this shader does not
// read it.

#include "environment.slang"
#include "fullscreen.slang"
#include "sanitize.slang"

static const uint kSampleCount = 1024u;

float VisibilitySmithGgxCorrelated(float NdotV, float NdotL, float alpha)
{
    const float a2 = alpha * alpha;
    const float ggxV = NdotL * sqrt(NdotV * NdotV * (1.0 - a2) + a2);
    const float ggxL = NdotV * sqrt(NdotL * NdotL * (1.0 - a2) + a2);
    return 0.5 / max(ggxV + ggxL, 1.0e-7);
}

[shader("fragment")]
float4 FragmentMain(FullscreenVaryings input) : SV_Target
{
    const float NdotV = input.uv.x;
    const float alpha = input.uv.y * input.uv.y;
    const float3 v = float3(sqrt(1.0 - NdotV * NdotV), 0.0, NdotV);
    float2 sum = float2(0.0);
    for (uint i = 0u; i < kSampleCount; ++i) {
        const float3 h = ImportanceSampleGgx(Hammersley(i, kSampleCount), alpha);
        const float VdotH = dot(v, h);
        const float3 l = 2.0 * VdotH * h - v;
        const float NdotL = l.z;
        const float NdotH = h.z;
        if (NdotL <= 0.0 || VdotH <= 0.0) {
            continue;
        }
        // BRDF * NdotL / pdf, pdf = D * NdotH / (4 VdotH); D cancels.
        const float g = 4.0 * VisibilitySmithGgxCorrelated(NdotV, NdotL, alpha) *
                        NdotL * VdotH / max(NdotH, 1.0e-7);
        const float fc = pow(1.0 - VdotH, 5.0);
        sum += float2((1.0 - fc) * g, fc * g);
    }
    // Scale and bias are fractions of the reflected energy.
    return float4(SanitizeColor(sum / float(kSampleCount), 1.0), 0.0, 1.0);
}
