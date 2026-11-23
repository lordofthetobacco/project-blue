// Image-based lighting helpers shared by the environment filtering shaders
// (env_*.frag.metal), the skybox and the PBR shading. Included with
// #include "environment.h" after <metal_stdlib>; safe to include more than
// once. Functions are qualified with metal:: so that the header does not
// depend on the including file's `using namespace metal;`.
//
// Equirect mapping (matches src/lighting/environment.h): for a unit
// direction d, u = 0.5 + atan(d.x, -d.z) / (2 pi), v = acos(d.y) / pi, with
// (0, 0) the top-left corner of the image.
#ifndef BLUE_SHADERS_METAL_ENVIRONMENT_H_
#define BLUE_SHADERS_METAL_ENVIRONMENT_H_

constant float kEnvPi = 3.14159265359;
// Rec. 709 luminance weights (as lighting::EstimateSun).
constant float3 kLuminanceWeights = float3(0.2126, 0.7152, 0.0722);

// Returns `v` rotated by `angle` radians about +Y (right-handed, as
// glm::angleAxis(angle, vec3(0, 1, 0))).
static inline float3 RotateY(float3 v, float angle)
{
    const float c = metal::cos(angle);
    const float s = metal::sin(angle);
    return float3(c * v.x + s * v.z, v.y, -s * v.x + c * v.z);
}

// Returns the world direction of texel coordinate `uv` (0..1, top-left
// origin) on cube face `face` (+X, -X, +Y, -Y, +Z, -Z, Metal's slice order),
// following the cube map face selection rules (the orientation of each face
// in a texturecube).
static inline float3 CubeFaceDirection(int face, float2 uv)
{
    const float2 st = uv * 2.0 - 1.0;
    float3 d;
    if (face == 0) {
        d = float3(1.0, -st.y, -st.x);
    } else if (face == 1) {
        d = float3(-1.0, -st.y, st.x);
    } else if (face == 2) {
        d = float3(st.x, 1.0, st.y);
    } else if (face == 3) {
        d = float3(st.x, -1.0, -st.y);
    } else if (face == 4) {
        d = float3(st.x, -st.y, 1.0);
    } else {
        d = float3(-st.x, -st.y, -1.0);
    }
    return metal::normalize(d);
}

static inline float2 EquirectUv(float3 d)
{
    return float2(0.5 + metal::atan2(d.x, -d.z) / (2.0 * kEnvPi),
                  metal::acos(metal::clamp(d.y, -1.0, 1.0)) / kEnvPi);
}

// Returns the i-th of `count` points of the Hammersley set.
static inline float2 Hammersley(uint i, uint count)
{
    return float2(float(i) / float(count),
                  float(metal::reverse_bits(i)) * 2.3283064365386963e-10);
}

// Returns a GGX-distributed half vector (alpha = roughness^2) around +Z for
// the uniform sample `xi`. Mirrored by render::brdf_reference.
//
// cos^2(theta) is clamped to [0, 1] before both square roots: for small
// alpha the numerator and denominator round to the same float (alpha^4 * xi.y
// is below half an ulp of 1), and GPU division (x * rcp(y) on some GPUs,
// 2.5 ulp in the Metal Shading Language) can then return 1 + ulp, which
// made sinTheta = sqrt(-tiny) = NaN and poisoned whole rows of the BRDF
// LUT. At alpha = 0 and xi.y = 1 the quotient is 0 / 0.
static inline float3 ImportanceSampleGgx(float2 xi, float alpha)
{
    const float phi = 2.0 * kEnvPi * xi.x;
    const float denominator = metal::max(1.0 + (alpha * alpha - 1.0) * xi.y, 1.0e-20);
    const float cosThetaSq = metal::clamp((1.0 - xi.y) / denominator, 0.0, 1.0);
    const float cosTheta = metal::sqrt(cosThetaSq);
    const float sinTheta = metal::sqrt(metal::max(1.0 - cosThetaSq, 0.0));
    return float3(sinTheta * metal::cos(phi), sinTheta * metal::sin(phi), cosTheta);
}

// Returns an orthonormal basis whose third column is `n`.
static inline metal::float3x3 TangentFrame(float3 n)
{
    const float3 up = metal::abs(n.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    const float3 t = metal::normalize(metal::cross(up, n));
    return metal::float3x3(t, metal::cross(n, t), n);
}

#endif  // BLUE_SHADERS_METAL_ENVIRONMENT_H_
