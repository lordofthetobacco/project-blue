// Declarations shared by the reflex sight shaders. Included with
// #include "reticle.glsl" after common.glsl.

// Must match ReticleConstants in src/render/vulkan/passes/reticle_pass.cc.
layout(push_constant) uniform ReticleConstants {
    mat4 lens;     // Unit disc (xy in [-1, 1], z = 0) to world: the lens window.
    vec4 axis;     // xyz: optic axis, world, unit, pointing downrange.
                   // w: angular radius of the dot, radians.
    vec4 dotColor; // rgb: linear colour times intensity.
                   // w: smallest dot diameter on screen, pixels.
    vec4 tint;     // rgb: faint light added over the whole window.
} sight;
