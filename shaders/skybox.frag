#version 450

// The environment drawn as the sky behind everything (render::SkyboxPass):
// the unfiltered sky cube in the view direction, rotated and scaled like the
// environment lighting.

#include "common.glsl"
#include "environment.glsl"

layout(set = 1, binding = 0) uniform samplerCube skyMap;

layout(location = 0) in vec2 inNdc;
layout(location = 0) out vec4 outColor;

void main()
{
    // View-space direction through this pixel (any point on the ray works;
    // depth 1 is the near plane in reversed Z), then rotated to world space.
    const vec4 point = frame.inverseProjection * vec4(inNdc, 1.0, 1.0);
    const vec3 viewDirection = point.xyz / point.w;
    const vec3 world = normalize(transpose(mat3(frame.view)) * viewDirection);
    const vec3 d = RotateY(world, -frame.environmentParams.y);
    outColor = vec4(textureLod(skyMap, d, 0.0).rgb * frame.environmentParams.x, 1.0);
}
