#include <metal_stdlib>

using namespace metal;

// TODO(metal-stage4): removed with the scene pass. A test pattern for the
// HDR image while there is no scene: red rises from left to right, green
// from top to bottom, so a flipped or mirrored image, a wrong channel order
// or a missing sRGB encode is obvious in a screenshot.

// Must match FullscreenVaryings in fullscreen.vert.metal.
struct FullscreenVaryings {
    float4 position [[position]];
    float2 uv [[user(loc0)]];
};

fragment float4 FragmentMain(FullscreenVaryings varyings [[stage_in]])
{
    return float4(varyings.uv.x, varyings.uv.y, 0.0, 1.0);
}
