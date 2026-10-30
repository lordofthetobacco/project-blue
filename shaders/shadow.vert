#version 450

// Depth-only pass of one shadow cascade.
layout(push_constant) uniform ShadowConstants {
    mat4 lightModelViewProj; // cascade view-projection * model
} shadow;

layout(location = 0) in vec3 inPosition;

void main()
{
    gl_Position = shadow.lightModelViewProj * vec4(inPosition, 1.0);
}
