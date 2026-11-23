#include <metal_stdlib>

using namespace metal;

// Full-screen triangle at reversed-Z depth 0 (infinitely far) for the
// skybox (render::SkyboxPass). Passes the NDC position on so the fragment
// shader can rebuild the view direction. Metal's NDC y points up, as the
// projection matrices assume, so the NDC position goes to the inverse
// projection unchanged.

// Must match SkyboxVaryings in skybox.frag.metal.
struct SkyboxVaryings {
    float4 position [[position]];
    float2 ndc [[user(loc0)]];
};

vertex SkyboxVaryings VertexMain(uint vertexId [[vertex_id]])
{
    const float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    SkyboxVaryings varyings;
    varyings.ndc = uv * 2.0 - 1.0;
    varyings.position = float4(varyings.ndc, 0.0, 1.0);
    return varyings;
}
