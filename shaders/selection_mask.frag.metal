#include <metal_stdlib>

#include "bindings.h"

using namespace metal;

// Coverage of the selected draw items (render::OverlayPass), drawn with
// depth.vert.metal: 0.5 where an item is hidden by the scene (pipeline
// without a depth test), 1 where it is visible (depth-tested pipeline, drawn
// second).

constant bool kVisible [[function_constant(kConstantSelectionVisible)]];

// Must match DepthVaryings in depth.vert.metal (the uvs are not read).
struct DepthVaryings {
    float4 position [[position]];
};

fragment float4 FragmentMain(DepthVaryings in [[stage_in]])
{
    return float4(kVisible ? 1.0 : 0.5);
}
