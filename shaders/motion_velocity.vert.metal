#include <metal_stdlib>

#include "bindings.h"

using namespace metal;

#include "common.h"

// Velocity of the moving draw items (render::MotionBlurPass): their position
// this frame and last frame in clip space.

// Must match VelocityConstants in src/render/passes/motion_blur_pass.mm.
struct VelocityConstants {
    float4x4 model;
    float4x4 previousModel;  // The item's model matrix last frame.
};

struct VelocityVertex {
    float3 position [[attribute(kAttributePosition)]];
};

// Must match VelocityVaryings in motion_velocity.frag.metal.
struct VelocityVaryings {
    // Tested EQUAL against the depth pre-pass (depth.vert.metal): the very
    // same expression, invariant, gives bit-identical depths.
    float4 position [[position, invariant]];
    float4 currentClip [[user(loc0)]];
    float4 previousClip [[user(loc1)]];
};

vertex VelocityVaryings VertexMain(VelocityVertex in [[stage_in]],
                                   constant FrameUniforms& frame [[buffer(kBufferFrame)]],
                                   constant VelocityConstants& draw [[buffer(kBufferDraw)]])
{
    const float4 worldPosition = draw.model * float4(in.position, 1.0);
    VelocityVaryings out;
    out.position = frame.viewProj * worldPosition;
    // Without the temporal scaler's jitter, which is not motion: the inverse
    // of render::JitterProjection(), and exactly out.position without jitter.
    out.currentClip = out.position;
    out.currentClip.xy -= float2(2.0, -2.0) * frame.jitter.xy * frame.screen.zw * out.position.w;
    out.previousClip = frame.previousViewProj * (draw.previousModel * float4(in.position, 1.0));
    return out;
}
