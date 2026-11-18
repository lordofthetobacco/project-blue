#version 450

// Debug lines (render::OverlayPass): flat colour, alpha-blended.

layout(location = 0) in vec4 inColor;

layout(location = 0) out vec4 outColor;

void main()
{
    outColor = inColor;
}
