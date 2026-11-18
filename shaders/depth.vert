#version 450

#include "common.glsl"

// Depth pre-pass of the camera view (render::DepthPrepass).

// Must match DrawConstants in src/render/draw_list.cc.
layout(push_constant) uniform DrawConstants {
    mat4 model;
} draw;

layout(location = 0) in vec3 inPosition;
layout(location = 2) in vec2 inUv0;
layout(location = 4) in vec2 inUv1;

layout(location = 0) out vec4 outUv;  // xy: uv0, zw: uv1

// Must produce exactly the depth of mesh.vert: same inputs, same expression
// (worldPosition, then viewProj * worldPosition), both invariant.
invariant gl_Position;

void main()
{
    const vec4 worldPosition = draw.model * vec4(inPosition, 1.0);
    outUv = vec4(inUv0, inUv1);
    gl_Position = frame.viewProj * worldPosition;
}
