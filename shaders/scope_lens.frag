#version 450

#include "common.glsl"
#include "scope_lens.glsl"
#include "sanitize.glsl"

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

layout(location = 0) in vec2 inLensPosition;
layout(location = 1) in vec3 inWorldPosition;

layout(location = 0) out vec4 outColor;

// Returns 1 at or below edge0, 0 at or above edge1, smooth between.
float SmoothFall(float edge0, float edge1, float x)
{
    return 1.0 - smoothstep(edge0, edge1, x);
}

// Returns the undistorted apparent tangent shown at `t` for one colour
// channel (`aberration` is +ca red, 0 green, -ca blue).
vec2 DistortApparent(vec2 t, float aberration)
{
    const float r = length(t) / max(scope.up.w, 1.0e-6);
    return t * (1.0 + (scope.lensParams.x + aberration) * r * r);
}

// Returns how far `p` (apparent tangent) lies outside the nearest reticle
// line, each widened to at least `minHalfWidth`.
float ReticleDistance(vec2 p, float minHalfWidth)
{
    float nearest = 1.0e3;
    const int count = min(int(scope.reticleColor.w), kMaxReticleLines);
    for (int i = 0; i < count; ++i) {
        const vec2 from = scope.reticleLines[i].xy;
        const vec2 along = scope.reticleLines[i].zw - from;
        const float lengthSq = dot(along, along);
        const float s = lengthSq > 0.0 ? clamp(dot(p - from, along) / lengthSq, 0.0, 1.0) : 0.0;
        const float halfWidth = max(scope.reticleHalfWidths[i / 4][i % 4], minHalfWidth);
        nearest = min(nearest, length(p - (from + s * along)) - halfWidth);
    }
    return nearest;
}

// Returns one colour channel of the picture with the reticle at apparent
// tangent `t`; `pixel` is the apparent tangent one pixel spans.
float Channel(vec2 t, float aberration, int channel, float pixel)
{
    const vec2 source = DistortApparent(t, aberration);
    const vec2 world = source / max(scope.right.w, 1.0);
    const vec2 ndc = world / max(scope.axis.w, 1.0e-6);
    // The picture's rows run top-down (the scene pass's Y-up viewport).
    const vec2 uv = vec2(0.5 + 0.5 * ndc.x, 0.5 - 0.5 * ndc.y) * scope.lensParams.w;
    const float picture = texture(scopePicture, uv)[channel];
    const float line = ReticleDistance(source, 0.5 * scope.pupils.w * pixel);
    const float onLine = SmoothFall(-0.5 * pixel, 0.5 * pixel, line);
    return mix(picture, scope.reticleColor[channel], onLine);
}

void main()
{
    const float lensRadius = length(inLensPosition);
    const float rimWidth = max(fwidth(lensRadius), 1.0e-6);
    // Antialiased rim of the glass: the alpha of the blend.
    const float window = SmoothFall(1.0 - rimWidth, 1.0, lensRadius);

    const vec3 direction = normalize(inWorldPosition - frame.cameraPosition.xyz);
    const float along = max(dot(direction, scope.axis.xyz), 1.0e-4);
    const vec2 t = vec2(dot(direction, scope.right.xyz), dot(direction, scope.up.xyz)) / along;
    const float pixel = max(length(fwidth(t)), 1.0e-7);

    vec3 color = vec3(0.0);
    float lit = 0.0;
    if (scope.pupils.z > 0.5) {
        const float field = max(scope.up.w, 1.0e-6);
        // The field stop, antialiased.
        const float stop = SmoothFall(field - 0.5 * pixel, field + 0.5 * pixel, length(t));
        // Vignetting, quartic towards the edge.
        const float r = min(length(t) / field, 1.0);
        const float vignette = 1.0 - clamp(scope.lensParams.z, 0.0, 1.0) * r * r * r * r;
        // The eye box: where the ray to the eye crosses the exit pupil's plane.
        const vec2 crossing = scope.eye.xy + t * (scope.eye.z + scope.eye.w);
        const float exitPupil = scope.pupils.x;
        const float eyePupil = max(scope.pupils.y, 1.0e-5);
        const float eyeBox = SmoothFall(exitPupil, exitPupil + eyePupil, length(crossing));
        lit = stop * vignette * eyeBox;
        const float ca = scope.lensParams.y;
        color = vec3(Channel(t, ca, 0, pixel), Channel(t, 0.0, 1, pixel),
                     Channel(t, -ca, 2, pixel)) * lit;
    }
    // A faint reflection where the glass shows little.
    color += scope.glassTint.rgb * (1.0 - lit);
    outColor = vec4(SanitizeRadiance(color), window);
}
