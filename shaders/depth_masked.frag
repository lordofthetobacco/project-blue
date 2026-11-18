#version 450

#include "material.glsl"

// Alpha test of the depth pre-pass for alpha-masked materials: the same
// cutoff as mesh.frag, so the pre-pass keeps exactly the fragments the mesh
// pass shades. Writes depth only.

layout(location = 0) in vec2 inUv;

void main()
{
    const float alpha = texture(baseColorMap, inUv * material.uvAlpha.xy).a * material.baseColor.a;
    if (alpha < material.uvAlpha.z) {
        discard;
    }
}
