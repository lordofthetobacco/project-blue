// Declarations shared by the scope eyepiece shaders (render::ScopeLensPass).
// Included with #include "scope_lens.glsl" after common.glsl.

// Must match the line count of ScopeLensUniforms in
// src/render/vulkan/passes/scope_lens_pass.cc (render::kMaxScopeReticleLines).
const int kMaxReticleLines = 8;

// Must match ScopeLensUniforms in src/render/vulkan/passes/scope_lens_pass.cc
// (std140; offsets asserted there). Tangents are of directions against the
// scope's frame: t = (d.right, d.up) / (d.axis).
layout(set = 1, binding = 0) uniform ScopeLensUniforms {
    mat4 lens;          // Unit disc (xy in [-1, 1], z = 0) to world: the glass.
    vec4 right;         // xyz: scope right, world. w: magnification.
    vec4 up;            // xyz: scope up, world. w: apparent half field (tan).
    vec4 axis;          // xyz: scope axis, downrange. w: picture half field (tan).
    vec4 lensParams;    // x: distortion, y: chromatic aberration,
                        // z: vignetting, w: picture uv scale (used share).
    vec4 eye;           // xy: eye lateral offset (m), z: relief error (m),
                        // w: pupil shadow depth (m).
    vec4 pupils;        // x: exit pupil radius (m), y: eye pupil radius (m),
                        // z: 1 when a picture was rendered, w: min line pixels.
    vec4 reticleColor;  // rgb: linear colour, w: line count.
    vec4 glassTint;     // rgb: light the glass reflects where it shows nothing.
    vec4 reticleLines[kMaxReticleLines];   // xy from, zw to (apparent tangent).
    vec4 reticleHalfWidths[kMaxReticleLines / 4]; // Apparent tangent, 4 per vec4.
} scope;

// The scope's picture: linear radiance, sampled in [0, lensParams.w]^2.
layout(set = 1, binding = 1) uniform sampler2D scopePicture;
