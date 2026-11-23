#include <metal_stdlib>

#include "bindings.h"

using namespace metal;

#include "common.h"
#include "motion_vectors.h"

// Motion vectors of everything that stands still in the world
// (render::MotionVectorPass): each pixel's surface point, found again from
// the depth pre-pass, moves on screen only because the camera did. One
// triangle covers the static geometry and the sky; the items that moved are
// drawn over it (motion_object.*).

// Must match FullscreenVaryings in fullscreen.vert.metal.
struct FullscreenVaryings {
    float4 position [[position]];
    float2 uv [[user(loc0)]];
};

fragment float4 FragmentMain(FullscreenVaryings varyings [[stage_in]],
                             constant FrameUniforms& frame [[buffer(kBufferFrame)]],
                             depth2d<float> depthImage [[texture(kTextureInput0)]])
{
    // The pixel centre in the clip space the depth was rasterized in (with
    // the jitter); clip y points up.
    const float2 uv = varyings.position.xy * frame.screen.zw;
    const float depth = depthImage.read(uint2(varyings.position.xy));
    // Kept homogeneous: with the reversed-Z infinite projection w is
    // depth / near, so the sky (depth 0) comes out as a direction (w = 0)
    // pointing away from the camera, which the view-projections below turn
    // into the pixel it is seen at. No division, so no precision is lost on
    // far geometry.
    const float4 world = frame.inverseViewProj * float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);
    return float4(MotionVector(frame.unjitteredViewProj * world, frame.previousViewProj * world), 0.0, 0.0);
}
