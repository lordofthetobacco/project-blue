#version 450

#include "common.glsl"

// Must match DrawConstants in src/render/draw_list.cc.
layout(push_constant) uniform DrawConstants {
    mat4 model;
} draw;

// The mesh pass tests EQUAL against the depth pre-pass (depth.vert), which
// computes gl_Position with the very same expression: invariant makes the
// compiler produce bit-identical results for both.
invariant gl_Position;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUv;
layout(location = 3) in vec4 inTangent; // xyz: +u direction, w: handedness

layout(location = 0) out vec3 outWorldPosition;
layout(location = 1) out vec3 outNormal;
layout(location = 2) out vec2 outUv;
layout(location = 3) out vec4 outTangent;

void main()
{
    const vec4 worldPosition = draw.model * vec4(inPosition, 1.0);

    // Normals transform by the inverse transpose of the model's 3x3, which is
    // its cofactor matrix divided by the determinant. The cofactor costs
    // three cross products; only the determinant's sign matters since the
    // normal is renormalized, and it also flips the handedness of mirroring
    // transforms. Tangents are surface directions: they take the plain 3x3.
    const mat3 m = mat3(draw.model);
    const mat3 cofactor = mat3(cross(m[1], m[2]), cross(m[2], m[0]), cross(m[0], m[1]));
    const float mirror = dot(m[0], cofactor[0]) < 0.0 ? -1.0 : 1.0;

    outWorldPosition = worldPosition.xyz;
    outNormal = cofactor * inNormal * mirror;
    outUv = inUv;
    outTangent = vec4(m * inTangent.xyz, inTangent.w * mirror);

    gl_Position = frame.viewProj * worldPosition;
}
