#version 450

// Generates the split-sum BRDF lookup table (render::Environment), the GPU
// twin of lighting::GenerateBrdfLut(): x = NdotV, y = perceptual roughness
// (row 0 smoothest), rg = (scale, bias) so that the specular IBL term is
// prefiltered * (F0 * scale + F90 * bias). Importance-sampled GGX with
// height-correlated Smith visibility.

#include "environment.glsl"
#include "sanitize.glsl"

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

const uint kSampleCount = 1024u;

float VisibilitySmithGgxCorrelated(float NdotV, float NdotL, float alpha)
{
    const float a2 = alpha * alpha;
    const float ggxV = NdotL * sqrt(NdotV * NdotV * (1.0 - a2) + a2);
    const float ggxL = NdotV * sqrt(NdotL * NdotL * (1.0 - a2) + a2);
    return 0.5 / max(ggxV + ggxL, 1.0e-7);
}

void main()
{
    const float NdotV = inUv.x;
    const float alpha = inUv.y * inUv.y;
    const vec3 v = vec3(sqrt(1.0 - NdotV * NdotV), 0.0, NdotV);
    vec2 sum = vec2(0.0);
    for (uint i = 0u; i < kSampleCount; ++i) {
        const vec3 h = ImportanceSampleGgx(Hammersley(i, kSampleCount), alpha);
        const float VdotH = dot(v, h);
        const vec3 l = 2.0 * VdotH * h - v;
        const float NdotL = l.z;
        const float NdotH = h.z;
        if (NdotL <= 0.0 || VdotH <= 0.0) {
            continue;
        }
        // BRDF * NdotL / pdf, pdf = D * NdotH / (4 VdotH); D cancels.
        const float g = 4.0 * VisibilitySmithGgxCorrelated(NdotV, NdotL, alpha) *
                        NdotL * VdotH / max(NdotH, 1.0e-7);
        const float fc = pow(1.0 - VdotH, 5.0);
        sum += vec2((1.0 - fc) * g, fc * g);
    }
    // Scale and bias are fractions of the reflected energy.
    outColor = vec4(SanitizeColor(sum / float(kSampleCount), 1.0), 0.0, 1.0);
}
