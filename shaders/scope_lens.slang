// A rifle scope's eyepiece (render ScopeLensPass). Like the reflex sight,
// what a pixel shows is a function of the direction from the eye through it
// only: the apparent tangent t of that direction against the scope's frame.
// The eyepiece maps the world seen along the axis at tangent w onto t = M w,
// so the picture (rendered along the scope's axis, square, half field
// axis.w) is sampled at w = t / M: it moves with the weapon, stays on target
// whatever the eye's position, and the reticle, in a focal plane, is fixed
// in it. Program: scope_lens.
//
// Mirrors src/weapon/scope_optics.cc (DistortApparent, Vignetting,
// EyeBoxTransmission, ReticleDistance); keep them in step.

#include "common.slang"
#include "sanitize.slang"

// Must match the line count of ScopeLensUniforms in
// src/render/vulkan/passes/scope_lens_pass.cc (render::kMaxScopeReticleLines).
static const int kMaxReticleLines = BLUE_MAX_SCOPE_RETICLE_LINES;

// Must match ScopeLensUniforms in src/render/vulkan/passes/scope_lens_pass.cc
// (only 16-byte members; offsets asserted there). Tangents are of directions
// against the scope's frame: t = (d.right, d.up) / (d.axis).
struct ScopeLensUniforms {
    float4x4 lens;        // Unit disc (xy in [-1, 1], z = 0) to world: the glass.
    float4 right;         // xyz: scope right, world. w: magnification.
    float4 up;            // xyz: scope up, world. w: apparent half field (tan).
    float4 axis;          // xyz: scope axis, downrange. w: picture half field (tan).
    float4 lensParams;    // x: distortion, y: chromatic aberration,
                          // z: vignetting, w: picture uv scale (used share).
    float4 eye;           // xy: eye lateral offset (m), z: relief error (m),
                          // w: pupil shadow depth (m).
    float4 pupils;        // x: exit pupil radius (m), y: eye pupil radius (m),
                          // z: 1 when a picture was rendered, w: min line pixels.
    float4 reticleColor;  // rgb: linear colour, w: line count.
    float4 glassTint;     // rgb: light the glass reflects where it shows nothing.
    float4 reticleLines[kMaxReticleLines];           // xy from, zw to (apparent tangent).
    float4 reticleHalfWidths[kMaxReticleLines / 4];  // Apparent tangent, 4 per float4.
};
BLUE_UNIFORMS(ScopeLensUniforms, scope, BLUE_SCOPE_UNIFORMS_BINDING, BLUE_PASS_SET);

// The scope's picture: linear radiance, sampled in [0, lensParams.w]^2.
BLUE_TEXTURE(Texture2D<float4>, scopePicture, BLUE_SCOPE_PICTURE_BINDING, BLUE_PASS_SET);
BLUE_SAMPLER(SamplerState, scopeSampler, BLUE_SCOPE_SAMPLER_BINDING, BLUE_PASS_SET);

// Spans the eyepiece glass with a quad of two triangles; the fragment shader
// trims it to a disc. No vertex buffer.
static const float2 kCorners[6] = {
    float2(-1.0, -1.0), float2(1.0, -1.0), float2(1.0, 1.0),
    float2(-1.0, -1.0), float2(1.0, 1.0),  float2(-1.0, 1.0)};

struct Varyings {
    float4 clipPosition : SV_Position;
    float2 lensPosition : TEXCOORD0;  // In the unit disc.
    float3 worldPosition : WORLDPOSITION;
};

[shader("vertex")]
Varyings VertexMain(uint vertexIndex : SV_VertexID)
{
    const float2 corner = kCorners[vertexIndex];
    const float4 worldPosition = mul(scope.lens, float4(corner, 0.0, 1.0));
    Varyings output;
    output.lensPosition = corner;
    output.worldPosition = worldPosition.xyz;
    output.clipPosition = mul(frame.viewProj, worldPosition);
    return output;
}

// Returns 1 at or below edge0, 0 at or above edge1, smooth between.
float SmoothFall(float edge0, float edge1, float x)
{
    return 1.0 - smoothstep(edge0, edge1, x);
}

// Returns the undistorted apparent tangent shown at `t` for one colour
// channel (`aberration` is +ca red, 0 green, -ca blue).
float2 DistortApparent(float2 t, float aberration)
{
    const float r = length(t) / max(scope.up.w, 1.0e-6);
    return t * (1.0 + (scope.lensParams.x + aberration) * r * r);
}

// Returns how far `p` (apparent tangent) lies outside the nearest reticle
// line, each widened to at least `minHalfWidth`.
float ReticleDistance(float2 p, float minHalfWidth)
{
    float nearest = 1.0e3;
    const int count = min(int(scope.reticleColor.w), kMaxReticleLines);
    for (int i = 0; i < count; ++i) {
        const float2 from = scope.reticleLines[i].xy;
        const float2 along = scope.reticleLines[i].zw - from;
        const float lengthSq = dot(along, along);
        const float s = lengthSq > 0.0 ? clamp(dot(p - from, along) / lengthSq, 0.0, 1.0) : 0.0;
        const float halfWidth = max(scope.reticleHalfWidths[i / 4][i % 4], minHalfWidth);
        nearest = min(nearest, length(p - (from + s * along)) - halfWidth);
    }
    return nearest;
}

// Returns one colour channel of the picture with the reticle at apparent
// tangent `t`; `pixel` is the apparent tangent one pixel spans.
float Channel(float2 t, float aberration, int channel, float pixel)
{
    const float2 source = DistortApparent(t, aberration);
    const float2 world = source / max(scope.right.w, 1.0);
    const float2 ndc = world / max(scope.axis.w, 1.0e-6);
    // The picture's rows run top-down (the scene pass's Y-up viewport).
    const float2 uv = float2(0.5 + 0.5 * ndc.x, 0.5 - 0.5 * ndc.y) * scope.lensParams.w;
    const float picture = scopePicture.Sample(scopeSampler, uv)[channel];
    const float line = ReticleDistance(source, 0.5 * scope.pupils.w * pixel);
    const float onLine = SmoothFall(-0.5 * pixel, 0.5 * pixel, line);
    return lerp(picture, scope.reticleColor[channel], onLine);
}

[shader("fragment")]
float4 FragmentMain(Varyings input) : SV_Target
{
    const float lensRadius = length(input.lensPosition);
    const float rimWidth = max(fwidth(lensRadius), 1.0e-6);
    // Antialiased rim of the glass: the alpha of the blend.
    const float window = SmoothFall(1.0 - rimWidth, 1.0, lensRadius);

    const float3 direction = normalize(input.worldPosition - frame.cameraPosition.xyz);
    const float along = max(dot(direction, scope.axis.xyz), 1.0e-4);
    const float2 t =
        float2(dot(direction, scope.right.xyz), dot(direction, scope.up.xyz)) / along;
    const float pixel = max(length(fwidth(t)), 1.0e-7);

    float3 color = float3(0.0);
    float lit = 0.0;
    if (scope.pupils.z > 0.5) {
        const float field = max(scope.up.w, 1.0e-6);
        // The field stop, antialiased.
        const float stop = SmoothFall(field - 0.5 * pixel, field + 0.5 * pixel, length(t));
        // Vignetting, quartic towards the edge.
        const float r = min(length(t) / field, 1.0);
        const float vignette = 1.0 - clamp(scope.lensParams.z, 0.0, 1.0) * r * r * r * r;
        // The eye box: where the ray to the eye crosses the exit pupil's plane.
        const float2 crossing = scope.eye.xy + t * (scope.eye.z + scope.eye.w);
        const float exitPupil = scope.pupils.x;
        const float eyePupil = max(scope.pupils.y, 1.0e-5);
        const float eyeBox = SmoothFall(exitPupil, exitPupil + eyePupil, length(crossing));
        lit = stop * vignette * eyeBox;
        const float ca = scope.lensParams.y;
        color = float3(Channel(t, ca, 0, pixel), Channel(t, 0.0, 1, pixel),
                       Channel(t, -ca, 2, pixel)) * lit;
    }
    // A faint reflection where the glass shows little.
    color += scope.glassTint.rgb * (1.0 - lit);
    return float4(SanitizeRadiance(color), window);
}
