// Declarations shared by the scope eyepiece shaders (render::ScopeLensPass).
// Included with #include "scope_lens.h" after <metal_stdlib> and
// "bindings.h"; safe to include more than once. Types are qualified with
// metal:: so that the header does not depend on the including file's `using
// namespace metal;`.
#ifndef BLUE_SHADERS_SCOPE_LENS_H_
#define BLUE_SHADERS_SCOPE_LENS_H_

// Must match the line count of ScopeLensUniforms in
// src/render/passes/scope_lens_pass.mm (render::kMaxScopeReticleLines).
constant int kMaxReticleLines = 8;

// The lens uniforms, at kBufferScopeLens of both stages (one buffer per frame
// in flight). Must match ScopeLensUniforms in
// src/render/passes/scope_lens_pass.mm (offsets asserted there; float4 and
// float4x4 members leave no padding). Tangents are of directions against the
// scope's frame: t = (d.right, d.up) / (d.axis).
struct ScopeLensUniforms {
    metal::float4x4 lens; // Unit disc (xy in [-1, 1], z = 0) to world: the glass.
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
    float4 reticleLines[kMaxReticleLines];          // xy from, zw to (apparent tangent).
    float4 reticleHalfWidths[kMaxReticleLines / 4]; // Apparent tangent, 4 per float4.
};
static_assert(sizeof(ScopeLensUniforms) == 352,
              "ScopeLensUniforms must match src/render/passes/scope_lens_pass.mm");

// Must match between scope_lens.vert.metal and scope_lens.frag.metal.
struct ScopeLensVaryings {
    float4 position [[position]];
    float2 lensPosition [[user(loc0)]];  // In the unit disc.
    float3 worldPosition [[user(loc1)]];
};

#endif  // BLUE_SHADERS_SCOPE_LENS_H_
