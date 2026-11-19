#include <metal_stdlib>

using namespace metal;

// Debug lines (render::OverlayPass): flat colour, alpha-blended.

// Must match LineVaryings in line.vert.metal.
struct LineVaryings {
    float4 position [[position]];
    float4 color [[user(loc0)]];
};

fragment float4 FragmentMain(LineVaryings in [[stage_in]])
{
    return in.color;
}
