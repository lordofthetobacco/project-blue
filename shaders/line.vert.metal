#include <metal_stdlib>

#include "bindings.h"

using namespace metal;

#include "common.h"

// Debug lines over the tonemapped image (render::OverlayPass). Colours are
// display-space (sRGB-encoded) RGBA8, written as they are.

struct LineVertex {
    float3 position [[attribute(kAttributePosition)]];  // world space
    float4 color [[attribute(kAttributeLineColor)]];    // RGBA8 unorm
};

// Must match LineVaryings in line.frag.metal.
struct LineVaryings {
    float4 position [[position]];
    float4 color [[user(loc0)]];
};

vertex LineVaryings VertexMain(LineVertex in [[stage_in]],
                               constant FrameUniforms& frame [[buffer(kBufferFrame)]])
{
    LineVaryings out;
    out.color = in.color;
    out.position = frame.viewProj * float4(in.position, 1.0);
    return out;
}
