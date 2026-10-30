#version 450

#include "common.glsl"

layout(push_constant) uniform DrawConstants {
    mat4 model;
    mat4 normalMatrix; // inverse-transpose of model, upper 3x3 used
} draw;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inColor;

layout(location = 0) out vec3 outWorldPosition;
layout(location = 1) out vec3 outNormal;
layout(location = 2) out vec3 outColor;

void main()
{
    const vec4 worldPosition = draw.model * vec4(inPosition, 1.0);

    outWorldPosition = worldPosition.xyz;
    outNormal = mat3(draw.normalMatrix) * inNormal;
    outColor = inColor;

    gl_Position = frame.viewProj * worldPosition;
}
