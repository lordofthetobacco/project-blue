// Declarations shared by the reflex sight shaders (render::ReticlePass).
// Included with #include "reticle.h" after <metal_stdlib> and "bindings.h";
// safe to include more than once. Types are qualified with metal:: so that
// the header does not depend on the including file's `using namespace
// metal;`.
#ifndef BLUE_SHADERS_RETICLE_H_
#define BLUE_SHADERS_RETICLE_H_

// The pass constants, at kBufferPass of both stages. Must match
// ReticleConstants in src/render/passes/reticle_pass.mm.
struct ReticleConstants {
    metal::float4x4 lens; // Unit disc (xy in [-1, 1], z = 0) to world: the lens window.
    float4 axis;          // xyz: optic axis, world, unit, pointing downrange.
                          // w: angular radius of the dot, radians.
    float4 dotColor;      // rgb: linear colour times intensity.
                          // w: smallest dot diameter on screen, pixels.
    float4 tint;          // rgb: faint light added over the whole window.
};
static_assert(sizeof(ReticleConstants) == 112,
              "ReticleConstants must match src/render/passes/reticle_pass.mm");

// Must match between reticle.vert.metal and the reticle fragment shaders.
struct ReticleVaryings {
    float4 position [[position]];
    float2 lensPosition [[user(loc0)]];  // In the unit disc.
    float3 worldPosition [[user(loc1)]];
};

// How much of a pixel the sight covers.
struct ReticleCoverage {
    float window;  // The lens window: 1 inside the disc, antialiased rim.
    float dot;     // The dot: 1 inside it, antialiased edge.
};

// Returns the coverage of the fragment `varyings` of the sight seen from
// `cameraPosition`. Fragment stage only (screen-space derivatives).
static inline ReticleCoverage ReticleCoverageOf(ReticleVaryings varyings, float3 cameraPosition,
                                                constant ReticleConstants& sight)
{
    const float lensRadius = metal::length(varyings.lensPosition);
    const float rimWidth = metal::max(metal::fwidth(lensRadius), 1.0e-6);
    const float3 direction = metal::normalize(varyings.worldPosition - cameraPosition);
    // Angle one pixel spans, from how fast the direction changes on screen.
    const float pixelAngle = metal::max(metal::length(metal::fwidth(direction)), 1.0e-7);

    ReticleCoverage coverage;
    // Antialiased rim of the window; zero outside the disc.
    coverage.window = 1.0 - metal::smoothstep(1.0 - rimWidth, 1.0, lensRadius);

    // Angle to the axis; the chord form stays accurate for tiny angles where
    // acos(dot) loses all precision.
    const float angle =
        2.0 * metal::asin(metal::min(0.5 * metal::length(direction - sight.axis.xyz), 1.0));
    const float radius = metal::max(sight.axis.w, 0.5 * sight.dotColor.w * pixelAngle);
    coverage.dot = 1.0 - metal::smoothstep(radius - 0.5 * pixelAngle,
                                           radius + 0.5 * pixelAngle, angle);
    return coverage;
}

#endif  // BLUE_SHADERS_RETICLE_H_
