#include <metal_stdlib>

#include "bindings.h"

using namespace metal;

#include "common.h"
#include "sanitize.h"
#include "scope_lens.h"

// A rifle scope's eyepiece. Like the reflex sight, what a pixel shows is a
// function of the direction from the eye through it only: the apparent
// tangent t of that direction against the scope's frame. The eyepiece maps
// the world seen along the axis at tangent w onto t = M w, so the picture
// (rendered along the scope's axis, square, half field axis.w) is sampled
// at w = t / M: it moves with the weapon, stays on target whatever the
// eye's position, and the reticle, in a focal plane, is fixed in it.
//
// Mirrors src/weapon/scope_optics.cc (DistortApparent, Vignetting,
// EyeBoxTransmission, ReticleDistance); keep them in step.

// Returns 1 at or below edge0, 0 at or above edge1, smooth between.
static inline float SmoothFall(float edge0, float edge1, float x)
{
    return 1.0 - smoothstep(edge0, edge1, x);
}

// Returns the undistorted apparent tangent shown at `t` for one colour
// channel (`aberration` is +ca red, 0 green, -ca blue).
static inline float2 DistortApparent(constant ScopeLensUniforms& scope, float2 t, float aberration)
{
    const float r = length(t) / max(scope.up.w, 1.0e-6);
    return t * (1.0 + (scope.lensParams.x + aberration) * r * r);
}

// Returns how far `p` (apparent tangent) lies outside the nearest reticle
// line, each widened to at least `minHalfWidth`.
static inline float ReticleDistance(constant ScopeLensUniforms& scope, float2 p, float minHalfWidth)
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
static inline float Channel(constant ScopeLensUniforms& scope, texture2d<float> scopePicture,
                            sampler linearClamp, float2 t, float aberration, int channel,
                            float pixel)
{
    const float2 source = DistortApparent(scope, t, aberration);
    const float2 world = source / max(scope.right.w, 1.0);
    const float2 ndc = world / max(scope.axis.w, 1.0e-6);
    // Metal's texture origin is the top-left corner while the picture's NDC
    // y points up (the scene pass renders it with a plain viewport).
    const float2 uv = float2(0.5 + 0.5 * ndc.x, 0.5 - 0.5 * ndc.y) * scope.lensParams.w;
    const float picture = scopePicture.sample(linearClamp, uv)[channel];
    const float line = ReticleDistance(scope, source, 0.5 * scope.pupils.w * pixel);
    const float onLine = SmoothFall(-0.5 * pixel, 0.5 * pixel, line);
    return mix(picture, scope.reticleColor[channel], onLine);
}

// scopePicture: the scope's picture, linear radiance, sampled in
// [0, lensParams.w]^2.
fragment float4 FragmentMain(ScopeLensVaryings varyings [[stage_in]],
                             constant FrameUniforms& frame [[buffer(kBufferFrame)]],
                             constant ScopeLensUniforms& scope [[buffer(kBufferScopeLens)]],
                             texture2d<float> scopePicture [[texture(kTextureScopePicture)]],
                             sampler linearClamp [[sampler(kSamplerLinearClamp)]])
{
    const float lensRadius = length(varyings.lensPosition);
    const float rimWidth = max(fwidth(lensRadius), 1.0e-6);
    // Antialiased rim of the glass: the alpha of the blend.
    const float window = SmoothFall(1.0 - rimWidth, 1.0, lensRadius);

    const float3 direction = normalize(varyings.worldPosition - frame.cameraPosition.xyz);
    const float along = max(dot(direction, scope.axis.xyz), 1.0e-4);
    const float2 t = float2(dot(direction, scope.right.xyz), dot(direction, scope.up.xyz)) / along;
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
        color = float3(Channel(scope, scopePicture, linearClamp, t, ca, 0, pixel),
                       Channel(scope, scopePicture, linearClamp, t, 0.0, 1, pixel),
                       Channel(scope, scopePicture, linearClamp, t, -ca, 2, pixel)) * lit;
    }
    // A faint reflection where the glass shows little.
    color += scope.glassTint.rgb * (1.0 - lit);
    return float4(SanitizeRadiance(color), window);
}
