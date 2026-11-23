#include <metal_stdlib>

#include "bindings.h"

using namespace metal;

#include "common.h"

// Depth pre-pass of the camera view (render::DepthPrepass); also feeds
// depth_masked.frag.metal and the editor's selection mask.

// Must match DrawConstants in src/render/draw_list.mm.
struct DrawConstants {
    float4x4 model;
};

struct DepthVertex {
    float3 position [[attribute(kAttributePosition)]];
    float2 uv0 [[attribute(kAttributeUv0)]];
    float2 uv1 [[attribute(kAttributeUv1)]];
};

// Must match DepthVaryings in depth_masked.frag.metal.
struct DepthVaryings {
    // Must produce exactly the depth of mesh.vert.metal: same inputs, same
    // expression (worldPosition, then viewProj * worldPosition), both
    // invariant.
    float4 position [[position, invariant]];
    float4 uv [[user(loc0)]];  // xy: uv0, zw: uv1
};

vertex DepthVaryings VertexMain(DepthVertex in [[stage_in]],
                                constant FrameUniforms& frame [[buffer(kBufferFrame)]],
                                constant DrawConstants& draw [[buffer(kBufferDraw)]])
{
    const float4 worldPosition = draw.model * float4(in.position, 1.0);
    DepthVaryings out;
    out.uv = float4(in.uv0, in.uv1);
    out.position = frame.viewProj * worldPosition;
    return out;
}
