// Image-based lighting helpers shared by the environment filtering shaders
// (env_*.slang), the skybox and pbr.slang. Included with
// #include "environment.slang"; safe to include more than once.
//
// Equirect mapping (matches src/lighting/environment.h): for a unit
// direction d, u = 0.5 + atan2(d.x, -d.z) / (2 pi), v = acos(d.y) / pi, with
// (0, 0) the top-left corner of the image.
#ifndef BLUE_SHADERS_ENVIRONMENT_SLANG_
#define BLUE_SHADERS_ENVIRONMENT_SLANG_

static const float kEnvPi = 3.14159265359;
// Rec. 709 luminance weights (as lighting::EstimateSun).
static const float3 kLuminanceWeights = float3(0.2126, 0.7152, 0.0722);

// Returns `v` rotated by `angle` radians about +Y (right-handed, as
// glm::angleAxis(angle, vec3(0, 1, 0))).
float3 RotateY(float3 v, float angle)
{
    const float c = cos(angle);
    const float s = sin(angle);
    return float3(c * v.x + s * v.z, v.y, -s * v.x + c * v.z);
}

// Returns the world direction of texel coordinate `uv` (0..1, top-left
// origin) on cube face `face` (+X, -X, +Y, -Y, +Z, -Z), following the
// Vulkan cube map face selection rules (Direct3D's are the same).
float3 CubeFaceDirection(int face, float2 uv)
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
    return normalize(d);
}

float2 EquirectUv(float3 d)
{
    return float2(0.5 + atan2(d.x, -d.z) / (2.0 * kEnvPi),
                  acos(clamp(d.y, -1.0, 1.0)) / kEnvPi);
}

// Returns the i-th of `count` points of the Hammersley set.
float2 Hammersley(uint i, uint count)
{
    return float2(float(i) / float(count),
                  float(reversebits(i)) * 2.3283064365386963e-10);
}

// Returns a GGX-distributed half vector (alpha = roughness^2) around +Z for
// the uniform sample `xi`. Mirrored by render::brdf_reference.
//
// cos^2(theta) is clamped to [0, 1] before both square roots: for small
// alpha the numerator and denominator round to the same float (alpha^4 * xi.y
// is below half an ulp of 1), and GPU division (x * rcp(y), 2.5 ulp in
// Vulkan) can then return 1 + ulp, which made sinTheta = sqrt(-tiny) = NaN
// and poisoned whole rows of the BRDF LUT. At alpha = 0 and xi.y = 1 the
// quotient is 0 / 0.
float3 ImportanceSampleGgx(float2 xi, float alpha)
{
    const float phi = 2.0 * kEnvPi * xi.x;
    const float denominator = max(1.0 + (alpha * alpha - 1.0) * xi.y, 1.0e-20);
    const float cosThetaSq = clamp((1.0 - xi.y) / denominator, 0.0, 1.0);
    const float cosTheta = sqrt(cosThetaSq);
    const float sinTheta = sqrt(max(1.0 - cosThetaSq, 0.0));
    return float3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);
}

// Returns `v`, given in an orthonormal tangent frame whose +Z is `n`, in
// the space of `n`.
float3 FromTangentFrame(float3 n, float3 v)
{
    const float3 up = abs(n.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    const float3 t = normalize(cross(up, n));
    const float3 b = cross(n, t);
    return t * v.x + b * v.y + n * v.z;
}

#endif  // BLUE_SHADERS_ENVIRONMENT_SLANG_
