#include <metal_stdlib>

#include "bindings.h"

using namespace metal;

#include "common.h"

// The draw items that moved since the previous frame, for their motion
// vectors (render::MotionVectorPass): their position this frame and last
// frame in clip space, without the jitter.

// Must match ObjectMotionConstants in src/render/passes/motion_vector_pass.mm.
struct ObjectMotionConstants {
    float4x4 model;
    float4x4 previousModel;  // The item's model matrix last frame.
};

struct ObjectMotionVertex {
    float3 position [[attribute(kAttributePosition)]];
};

// Must match ObjectMotionVaryings in motion_object.frag.metal.
struct ObjectMotionVaryings {
    // Tested EQUAL against the depth pre-pass (depth.vert.metal): the very
    // same expression, invariant, gives bit-identical depths.
    float4 position [[position, invariant]];
    float4 clip [[user(loc0)]];
    float4 previousClip [[user(loc1)]];
};

vertex ObjectMotionVaryings VertexMain(ObjectMotionVertex in [[stage_in]],
                                       constant FrameUniforms& frame [[buffer(kBufferFrame)]],
                                       constant ObjectMotionConstants& draw [[buffer(kBufferDraw)]])
{
    const float4 worldPosition = draw.model * float4(in.position, 1.0);
    ObjectMotionVaryings out;
    out.position = frame.viewProj * worldPosition;
    out.clip = frame.unjitteredViewProj * worldPosition;
    out.previousClip = frame.previousViewProj * (draw.previousModel * float4(in.position, 1.0));
    return out;
}
