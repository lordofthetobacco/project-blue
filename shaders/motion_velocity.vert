#version 450

#include "common.glsl"

// Velocity of the moving draw items (render::MotionBlurPass): their position
// this frame and last frame in clip space.

// Must match VelocityConstants in src/render/vulkan/passes/motion_blur_pass.cc.
layout(push_constant) uniform VelocityConstants {
    mat4 model;
    mat4 previousModel;  // The item's model matrix last frame.
} draw;

layout(location = 0) in vec3 inPosition;

layout(location = 0) out vec4 outCurrentClip;
layout(location = 1) out vec4 outPreviousClip;

// Tested EQUAL against the depth pre-pass (depth.vert): the very same
// expression, invariant, gives bit-identical depths.
invariant gl_Position;

void main()
{
    const vec4 worldPosition = draw.model * vec4(inPosition, 1.0);
    gl_Position = frame.viewProj * worldPosition;
    outCurrentClip = gl_Position;
    outPreviousClip = frame.previousViewProj * (draw.previousModel * vec4(inPosition, 1.0));
}
