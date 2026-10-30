#version 450

#include "common.glsl"

layout(location = 0) in vec3 inWorldPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inColor;

layout(location = 0) out vec4 outColor;

const float kShininess = 48.0;
const float kSpecularStrength = 0.35;

// Blinn-Phong lighting from a single point light, evaluated in world space.
void main()
{
    const vec3 normal = normalize(inNormal);
    const vec3 toLight = frame.lightPosition.xyz - inWorldPosition;
    const vec3 lightDir = normalize(toLight);
    const vec3 viewDir = normalize(frame.cameraPosition.xyz - inWorldPosition);
    const vec3 halfDir = normalize(lightDir + viewDir);

    const float distanceSq = dot(toLight, toLight);
    const vec3 radiance = frame.lightColor.rgb * frame.lightColor.a / distanceSq;

    const float diffuse = max(dot(normal, lightDir), 0.0);
    const float specular = diffuse > 0.0 ? pow(max(dot(normal, halfDir), 0.0), kShininess) : 0.0;

    const vec3 color = frame.ambientColor.rgb * inColor
                     + radiance * (diffuse * inColor + specular * kSpecularStrength);

    outColor = vec4(color, 1.0);
}
