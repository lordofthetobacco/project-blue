#version 450

#include "material.glsl"

// Alpha test of the depth pre-pass for alpha-masked materials: the same
// cutoff as mesh.frag, so the pre-pass keeps exactly the fragments the mesh
// pass shades. Writes depth only.

layout(location = 0) in vec4 inUv;  // xy: uv0, zw: uv1

void main()
{
    const float alpha = texture(baseColorMap, MaterialUv(kSlotBaseColor, inUv.xy, inUv.zw)).a * material.baseColor.a;
    if (alpha < material.uvAlpha.z) {
        discard;
    }
}
