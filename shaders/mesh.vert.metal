#include <metal_stdlib>

#include "bindings.h"

using namespace metal;

#include "common.h"

// Must match DrawConstants in src/render/draw_list.mm.
struct DrawConstants {
    float4x4 model;
};

struct MeshVertex {
    float3 position [[attribute(kAttributePosition)]];
    float3 normal [[attribute(kAttributeNormal)]];
    float2 uv0 [[attribute(kAttributeUv0)]];
    float4 tangent [[attribute(kAttributeTangent)]];  // xyz: +u0 direction, w: handedness
    float2 uv1 [[attribute(kAttributeUv1)]];
};

// Must match MeshVaryings in mesh.frag.metal.
struct MeshVaryings {
    // The mesh pass tests EQUAL against the depth pre-pass (depth.vert.metal),
    // which computes the position with the very same expression: invariant
    // makes the compiler produce bit-identical results for both.
    float4 position [[position, invariant]];
    float3 worldPosition [[user(loc0)]];
    float3 normal [[user(loc1)]];
    float4 uv [[user(loc2)]];  // xy: uv0, zw: uv1
    float4 tangent [[user(loc3)]];
};

vertex MeshVaryings VertexMain(MeshVertex in [[stage_in]],
                               constant FrameUniforms& frame [[buffer(kBufferFrame)]],
                               constant DrawConstants& draw [[buffer(kBufferDraw)]])
{
    const float4 worldPosition = draw.model * float4(in.position, 1.0);

    // Normals transform by the inverse transpose of the model's 3x3, which is
    // its cofactor matrix divided by the determinant. The cofactor costs
    // three cross products; only the determinant's sign matters since the
    // normal is renormalized, and it also flips the handedness of mirroring
    // transforms. Tangents are surface directions: they take the plain 3x3.
    const float3x3 m = float3x3(draw.model[0].xyz, draw.model[1].xyz, draw.model[2].xyz);
    const float3x3 cofactor = float3x3(cross(m[1], m[2]), cross(m[2], m[0]), cross(m[0], m[1]));
    const float mirror = dot(m[0], cofactor[0]) < 0.0 ? -1.0 : 1.0;

    MeshVaryings out;
    out.worldPosition = worldPosition.xyz;
    out.normal = cofactor * in.normal * mirror;
    out.uv = float4(in.uv0, in.uv1);
    out.tangent = float4(m * in.tangent.xyz, in.tangent.w * mirror);

    out.position = frame.viewProj * worldPosition;
    return out;
}
