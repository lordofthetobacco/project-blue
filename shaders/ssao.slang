// Screen-space ambient occlusion at half resolution (render SsaoPass).
// Reconstructs view-space positions from the depth pre-pass (reversed-Z,
// infinite far plane), normals from neighbouring depths, and tests a small
// hemisphere of points around each position against the depth buffer.
// Writes r = ambient visibility (1 = open), g = linear view depth (for the
// depth-aware blur). Program: ssao.

#include "fullscreen.slang"

BLUE_TEXTURE(Texture2D<float4>, depthImage, 0, BLUE_INPUT_SET);  // full resolution

// Must match SsaoConstants in src/render/vulkan/passes/ssao_pass.cc.
struct SsaoConstants {
    float4 projection;  // x: proj[0][0], y: proj[1][1], z: near plane
    float4 params;      // x: radius (m), y: intensity, z: max radius (full-res px)
    float4 fullSize;    // xy: full-resolution size, zw: its reciprocal
};
BLUE_PUSH_CONSTANTS(SsaoConstants, params);

static const int kSampleCount = 12;
static const float kGoldenAngle = 2.39996323;
static const float kTwoPi = 6.28318531;
// Depth stored for the sky (no geometry): far enough to never occlude.
static const float kSkyDepth = 1.0e6;
// Below this projected radius (full-res pixels) occlusion is not resolvable.
static const float kMinRadiusPixels = 1.0;

// Returns the view-space z (negative in front of the camera) of `depth`.
// Reversed-Z infinite projection: depth = near / -z, so depth 0 is the sky.
float ViewZ(float depth)
{
    return depth > 0.0 ? -params.projection.z / depth : -kSkyDepth;
}

// Returns the view-space position of full-resolution texel `texel`.
float3 ViewPosition(int2 texel)
{
    const int2 clamped = clamp(texel, int2(0), int2(params.fullSize.xy) - 1);
    const float z = ViewZ(depthImage.Load(int3(clamped, 0)).r);
    const float2 uv = (float2(clamped) + 0.5) * params.fullSize.zw;
    // The scene viewport is flipped: uv.y = 0 (top) is NDC y = +1.
    const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    return float3(ndc.x * -z / params.projection.x, ndc.y * -z / params.projection.y, z);
}

// Returns the uv of view-space point `p` (in front of the camera).
float2 ProjectToUv(float3 p)
{
    const float2 ndc = float2(params.projection.x * p.x, params.projection.y * p.y) / -p.z;
    return float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
}

// Interleaved gradient noise (Jimenez 2014): a per-pixel value in [0, 1)
// whose 3x3 neighbourhoods cover the range evenly, so the blur removes it.
float InterleavedGradientNoise(float2 pixel)
{
    return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

[shader("fragment")]
float4 FragmentMain(FullscreenVaryings input) : SV_Target
{
    const float2 fragCoord = input.position.xy;
    const int2 center = int2(fragCoord) * 2;
    const float3 position = ViewPosition(center);
    if (position.z <= -kSkyDepth * 0.5) {
        return float4(1.0, kSkyDepth, 0.0, 0.0);
    }

    // Normal from the neighbours on the side with the smaller depth step, so
    // silhouettes do not bend it.
    const float3 right = ViewPosition(center + int2(2, 0)) - position;
    const float3 left = position - ViewPosition(center - int2(2, 0));
    const float3 down = ViewPosition(center + int2(0, 2)) - position;
    const float3 up = position - ViewPosition(center - int2(0, 2));
    const float3 dx = abs(right.z) < abs(left.z) ? right : left;
    const float3 dy = abs(down.z) < abs(up.z) ? down : up;
    // A degenerate neighbourhood (zero or non-finite cross product) falls
    // back to a normal facing the camera instead of producing NaN, which the
    // blur would spread into square blocks.
    float3 normal = cross(dx, dy);
    const float normalLengthSq = dot(normal, normal);
    normal = normalLengthSq > 1.0e-30 && !isinf(normalLengthSq)
                 ? normal * rsqrt(normalLengthSq)
                 : -normalize(position);
    if (dot(normal, position) > 0.0) {
        normal = -normal;  // Face the camera (at the origin).
    }

    // World-space radius, shrunk where it would cover too much of the screen
    // (the weapon a few centimetres from the eye).
    const float distance = -position.z;
    const float radiusPixels =
        params.params.x * params.projection.y * 0.5 * params.fullSize.y / distance;
    if (radiusPixels < kMinRadiusPixels) {
        return float4(1.0, distance, 0.0, 0.0);
    }
    const float radius = params.params.x * min(1.0, params.params.z / radiusPixels);
    const float bias = 0.03 * radius;

    // Orthonormal basis around the normal (Duff et al. 2017).
    const float s = normal.z >= 0.0 ? 1.0 : -1.0;
    const float a = -1.0 / (s + normal.z);
    const float b = normal.x * normal.y * a;
    const float3 tangent = float3(1.0 + s * normal.x * normal.x * a, s * b, -s * normal.x);
    const float3 bitangent = float3(b, s + normal.y * normal.y * a, -normal.y);

    const float rotation = InterleavedGradientNoise(fragCoord) * kTwoPi;
    float occlusion = 0.0;
    for (int i = 0; i < kSampleCount; ++i) {
        // Spiral over the hemisphere, cosine-distributed, with lengths
        // growing so more samples sit close to the surface.
        const float t = (float(i) + 0.5) / float(kSampleCount);
        const float phi = float(i) * kGoldenAngle + rotation;
        const float sinTheta = sqrt(t);
        const float3 local = float3(cos(phi) * sinTheta, sin(phi) * sinTheta, sqrt(1.0 - t));
        const float scale = lerp(0.1, 1.0, t * t);
        const float3 samplePoint = position
            + (tangent * local.x + bitangent * local.y + normal * local.z) * radius * scale;

        if (samplePoint.z > -params.projection.z) {
            continue;  // In front of the near plane: nothing can cover it.
        }
        const float2 uv = ProjectToUv(samplePoint);
        if (any(uv < float2(0.0)) || any(uv > float2(1.0))) {
            continue;
        }
        const float sceneZ =
            ViewZ(depthImage.Load(int3(int2(uv * params.fullSize.xy), 0)).r);
        // Occluded when the depth buffer has a surface in front of the point;
        // the range check fades out surfaces far in front (a foreground
        // object over a distant wall casts no occlusion on it).
        const float occluded = sceneZ >= samplePoint.z + bias ? 1.0 : 0.0;
        const float range = smoothstep(0.0, 1.0, radius / abs(position.z - sceneZ));
        occlusion += occluded * range;
    }
    // clamp() and the NaN check keep a broken sample from poisoning the
    // blur and the shading (NaN compares false: it becomes fully open).
    float visibility = pow(clamp(1.0 - occlusion / float(kSampleCount), 0.0, 1.0),
                           params.params.y);
    if (!(visibility >= 0.0 && visibility <= 1.0)) {
        visibility = 1.0;
    }
    return float4(visibility, distance, 0.0, 0.0);
}
