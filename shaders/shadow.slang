// Depth-only pass of one shadow cascade (render ShadowPass). Program: shadow.

#include "bindings.slang"

// Must match ShadowConstants in src/render/vulkan/passes/shadow_pass.cc.
struct ShadowConstants {
    float4x4 lightModelViewProj;  // cascade view-projection * model
};
BLUE_PUSH_CONSTANTS(ShadowConstants, shadow);

[shader("vertex")]
float4 VertexMain([[vk::location(0)]] float3 position : POSITION) : SV_Position
{
    return mul(shadow.lightModelViewProj, float4(position, 1.0));
}
