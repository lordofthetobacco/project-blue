#include <metal_stdlib>

using namespace metal;

// One triangle that covers the whole viewport (render::FullscreenPass). No
// vertex buffer: vertex i is at (-1, 1), (3, 1), (-1, -3). Metal's clip y
// points up (clip y = 1 is the top row of the target) while uv, like texture
// coordinates, starts at the top-left corner, so clip y = 1 - 2 uv.y: uv
// (0, 0) is the top-left of the target, where texture reads and writes
// start. The flip mirrors the triangle's winding, which does not matter
// because fullscreen pipelines cull nothing.

// Must match the varyings of every fullscreen fragment shader.
struct FullscreenVaryings {
    float4 position [[position]];
    float2 uv [[user(loc0)]];
};

vertex FullscreenVaryings VertexMain(uint vertexId [[vertex_id]])
{
    const float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    FullscreenVaryings varyings;
    varyings.uv = uv;
    varyings.position = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
    return varyings;
}
