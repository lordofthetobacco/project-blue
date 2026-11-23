#include <metal_stdlib>

#include "bindings.h"

using namespace metal;

// Depth-only pass of one shadow cascade (render::ShadowPass).

// Must match ShadowConstants in src/render/passes/shadow_pass.mm.
struct ShadowConstants {
    float4x4 lightModelViewProj;  // cascade view-projection * model
};

struct ShadowVertex {
    float3 position [[attribute(kAttributePosition)]];
};

struct ShadowVaryings {
    float4 position [[position]];
};

vertex ShadowVaryings VertexMain(ShadowVertex in [[stage_in]],
                                 constant ShadowConstants& shadow [[buffer(kBufferDraw)]])
{
    ShadowVaryings out;
    out.position = shadow.lightModelViewProj * float4(in.position, 1.0);
    return out;
}
