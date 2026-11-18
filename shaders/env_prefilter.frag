#version 450

// GGX-prefilters one face of one mip of the specular environment cube
// (render::Environment): the split-sum prefiltered radiance with N = V = R
// (Karis 2013). Each sample reads the source mip whose texel solid angle
// matches the sample's (filtered importance sampling, Krivanek and Colbert
// 2008), which removes fireflies with few samples.

#include "environment.glsl"

layout(set = 0, binding = 0) uniform samplerCube source;

layout(push_constant) uniform PrefilterConstants {
    int face;
    float roughness;    // perceptual
    float sourceSize;   // texels per face of source mip 0
    int sampleCount;
} push;

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

float DistributionGgx(float NdotH, float alpha)
{
    const float a2 = alpha * alpha;
    const float d = (NdotH * a2 - NdotH) * NdotH + 1.0;
    return a2 / (kEnvPi * d * d);
}

void main()
{
    const vec3 n = CubeFaceDirection(push.face, inUv);
    if (push.sampleCount <= 1 || push.roughness <= 0.0) {
        outColor = vec4(textureLod(source, n, 0.0).rgb, 1.0);
        return;
    }
    const float alpha = push.roughness * push.roughness;
    const mat3 basis = TangentFrame(n);
    const float texelSolidAngle = 4.0 * kEnvPi / (6.0 * push.sourceSize * push.sourceSize);
    const uint count = uint(push.sampleCount);

    vec3 sum = vec3(0.0);
    float weight = 0.0;
    for (uint i = 0u; i < count; ++i) {
        const vec3 h = basis * ImportanceSampleGgx(Hammersley(i, count), alpha);
        const float NdotH = dot(n, h);
        const vec3 l = 2.0 * NdotH * h - n;  // V = N
        const float NdotL = dot(n, l);
        if (NdotL <= 0.0) {
            continue;
        }
        // pdf of l = D * NdotH / (4 VdotH) = D / 4 with V = N.
        const float pdf = DistributionGgx(max(NdotH, 0.0), alpha) * 0.25;
        const float sampleSolidAngle = 1.0 / (float(count) * pdf + 1.0e-6);
        const float lod = max(0.5 * log2(sampleSolidAngle / texelSolidAngle) + 1.0, 0.0);
        sum += textureLod(source, l, lod).rgb * NdotL;
        weight += NdotL;
    }
    outColor = vec4(sum / max(weight, 1.0e-6), 1.0);
}
