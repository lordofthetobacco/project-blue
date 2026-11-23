#include <metal_stdlib>

#include "bindings.h"

using namespace metal;

#include "common.h"
#include "scope_lens.h"

// Spans the eyepiece glass with a quad of two triangles; the fragment shader
// trims it to a disc. No vertex buffer.
constant float2 kCorners[6] = {
    float2(-1.0, -1.0), float2(1.0, -1.0), float2(1.0, 1.0),
    float2(-1.0, -1.0), float2(1.0, 1.0), float2(-1.0, 1.0)};

vertex ScopeLensVaryings VertexMain(uint vertexId [[vertex_id]],
                                    constant FrameUniforms& frame [[buffer(kBufferFrame)]],
                                    constant ScopeLensUniforms& scope [[buffer(kBufferScopeLens)]])
{
    const float2 corner = kCorners[vertexId];
    const float4 worldPosition = scope.lens * float4(corner, 0.0, 1.0);
    ScopeLensVaryings varyings;
    varyings.lensPosition = corner;
    varyings.worldPosition = worldPosition.xyz;
    varyings.position = frame.viewProj * worldPosition;
    return varyings;
}
