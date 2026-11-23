#include <metal_stdlib>

#include "bindings.h"

using namespace metal;

#include "common.h"
#include "reticle.h"

// Spans the lens window with a quad of two triangles; the fragment shader
// trims it to a disc. No vertex buffer.
constant float2 kCorners[6] = {
    float2(-1.0, -1.0), float2(1.0, -1.0), float2(1.0, 1.0),
    float2(-1.0, -1.0), float2(1.0, 1.0), float2(-1.0, 1.0)};

vertex ReticleVaryings VertexMain(uint vertexId [[vertex_id]],
                                  constant FrameUniforms& frame [[buffer(kBufferFrame)]],
                                  constant ReticleConstants& sight [[buffer(kBufferPass)]])
{
    const float2 corner = kCorners[vertexId];
    const float4 worldPosition = sight.lens * float4(corner, 0.0, 1.0);
    ReticleVaryings varyings;
    varyings.lensPosition = corner;
    varyings.worldPosition = worldPosition.xyz;
    varyings.position = frame.viewProj * worldPosition;
    return varyings;
}
