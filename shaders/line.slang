// Debug lines over the tonemapped image (render OverlayPass). Colours are
// display-space (sRGB-encoded) RGBA8, written as they are, alpha-blended.
// Program: line.

#include "common.slang"

struct VertexInput {
    [[vk::location(0)]] float3 position : POSITION;  // world space
    [[vk::location(1)]] float4 color : COLOR;        // R8G8B8A8_UNORM
};

struct Varyings {
    float4 clipPosition : SV_Position;
    float4 color : COLOR;
};

[shader("vertex")]
Varyings VertexMain(VertexInput input)
{
    Varyings output;
    output.color = input.color;
    output.clipPosition = mul(frame.viewProj, float4(input.position, 1.0));
    return output;
}

[shader("fragment")]
float4 FragmentMain(Varyings input) : SV_Target
{
    return input.color;
}
