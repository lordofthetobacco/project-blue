// Image-based lighting helpers shared by the environment filtering shaders
// (env_*.frag), the skybox and pbr.glsl. Included with
// #include "environment.glsl"; safe to include more than once.
//
// Equirect mapping (matches src/lighting/environment.h): for a unit
// direction d, u = 0.5 + atan(d.x, -d.z) / (2 pi), v = acos(d.y) / pi, with
// (0, 0) the top-left corner of the image.
#ifndef BLUE_SHADERS_ENVIRONMENT_GLSL_
#define BLUE_SHADERS_ENVIRONMENT_GLSL_

const float kEnvPi = 3.14159265359;
// Rec. 709 luminance weights (as lighting::EstimateSun).
const vec3 kLuminanceWeights = vec3(0.2126, 0.7152, 0.0722);

// Returns `v` rotated by `angle` radians about +Y (right-handed, as
// glm::angleAxis(angle, vec3(0, 1, 0))).
vec3 RotateY(vec3 v, float angle)
{
    const float c = cos(angle);
    const float s = sin(angle);
    return vec3(c * v.x + s * v.z, v.y, -s * v.x + c * v.z);
}

// Returns the world direction of texel coordinate `uv` (0..1, top-left
// origin) on cube face `face` (+X, -X, +Y, -Y, +Z, -Z), following the
// Vulkan cube map face selection rules.
vec3 CubeFaceDirection(int face, vec2 uv)
{
    const vec2 st = uv * 2.0 - 1.0;
    vec3 d;
    if (face == 0) {
        d = vec3(1.0, -st.y, -st.x);
    } else if (face == 1) {
        d = vec3(-1.0, -st.y, st.x);
    } else if (face == 2) {
        d = vec3(st.x, 1.0, st.y);
    } else if (face == 3) {
        d = vec3(st.x, -1.0, -st.y);
    } else if (face == 4) {
        d = vec3(st.x, -st.y, 1.0);
    } else {
        d = vec3(-st.x, -st.y, -1.0);
    }
    return normalize(d);
}

vec2 EquirectUv(vec3 d)
{
    return vec2(0.5 + atan(d.x, -d.z) / (2.0 * kEnvPi),
                acos(clamp(d.y, -1.0, 1.0)) / kEnvPi);
}

// Returns the i-th of `count` points of the Hammersley set.
vec2 Hammersley(uint i, uint count)
{
    return vec2(float(i) / float(count),
                float(bitfieldReverse(i)) * 2.3283064365386963e-10);
}

// Returns a GGX-distributed half vector (alpha = roughness^2) around +Z for
// the uniform sample `xi`.
vec3 ImportanceSampleGgx(vec2 xi, float alpha)
{
    const float phi = 2.0 * kEnvPi * xi.x;
    const float cosTheta = sqrt((1.0 - xi.y) / (1.0 + (alpha * alpha - 1.0) * xi.y));
    const float sinTheta = sqrt(1.0 - cosTheta * cosTheta);
    return vec3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);
}

// Returns an orthonormal basis whose third column is `n`.
mat3 TangentFrame(vec3 n)
{
    const vec3 up = abs(n.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    const vec3 t = normalize(cross(up, n));
    return mat3(t, cross(n, t), n);
}

#endif  // BLUE_SHADERS_ENVIRONMENT_GLSL_
