// The environment drawn as the sky behind everything (render SkyboxPass):
// the unfiltered sky cube in the view direction, rotated and scaled like the
// environment lighting. Program: skybox.

#include "common.slang"
#include "environment.slang"
#include "sanitize.slang"

BLUE_TEXTURE(TextureCube<float4>, skyMap, BLUE_SKY_CUBE_BINDING, BLUE_PASS_SET);
BLUE_SAMPLER(SamplerState, skySampler, BLUE_SKY_SAMPLER_BINDING, BLUE_PASS_SET);

struct Varyings {
    float4 clipPosition : SV_Position;
    float2 ndc : TEXCOORD0;
};

// Full-screen triangle at reversed-Z depth 0 (infinitely far). Passes the
// NDC position on so the fragment shader can rebuild the view direction.
[shader("vertex")]
Varyings VertexMain(uint vertexIndex : SV_VertexID)
{
    const float2 uv = float2(float((vertexIndex << 1) & 2u), float(vertexIndex & 2u));
    Varyings output;
    output.ndc = uv * 2.0 - 1.0;
    output.clipPosition = float4(output.ndc, 0.0, 1.0);
    return output;
}

[shader("fragment")]
float4 FragmentMain(Varyings input) : SV_Target
{
    // View-space direction through this pixel (any point on the ray works;
    // depth 1 is the near plane in reversed Z), then rotated to world space
    // by the transpose of the view's rotation.
    const float4 point = mul(frame.inverseProjection, float4(input.ndc, 1.0, 1.0));
    const float3 viewDirection = point.xyz / point.w;
    const float3 world = normalize(mul(viewDirection, (float3x3)frame.view));
    const float3 d = RotateY(world, -frame.environmentParams.y);
    const float3 sky = skyMap.SampleLevel(skySampler, d, 0.0).rgb;
    return float4(SanitizeRadiance(sky * frame.environmentParams.x), 1.0);
}
