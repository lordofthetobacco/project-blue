#version 450

#include "common.glsl"
#include "reticle.glsl"

// Spans the lens window with a quad of two triangles; the fragment shader
// trims it to a disc. No vertex buffer.
const vec2 kCorners[6] = vec2[](
    vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0),
    vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0));

layout(location = 0) out vec2 outLensPosition; // In the unit disc.
layout(location = 1) out vec3 outWorldPosition;

void main()
{
    const vec2 corner = kCorners[gl_VertexIndex];
    const vec4 worldPosition = sight.lens * vec4(corner, 0.0, 1.0);
    outLensPosition = corner;
    outWorldPosition = worldPosition.xyz;
    gl_Position = frame.viewProj * worldPosition;
}
