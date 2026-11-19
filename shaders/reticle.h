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

// Must match between reticle.vert.metal and reticle.frag.metal.
struct ReticleVaryings {
    float4 position [[position]];
    float2 lensPosition [[user(loc0)]];  // In the unit disc.
    float3 worldPosition [[user(loc1)]];
};

#endif  // BLUE_SHADERS_RETICLE_H_
