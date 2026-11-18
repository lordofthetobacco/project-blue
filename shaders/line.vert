#version 450

#include "common.glsl"

// Debug lines over the tonemapped image (render::OverlayPass). Colours are
// display-space (sRGB-encoded) RGBA8, written as they are.

layout(location = 0) in vec3 inPosition;  // world space
layout(location = 1) in vec4 inColor;     // R8G8B8A8_UNORM

layout(location = 0) out vec4 outColor;

void main()
{
    outColor = inColor;
    gl_Position = frame.viewProj * vec4(inPosition, 1.0);
}
