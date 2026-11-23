#include <metal_stdlib>

#include "bindings.h"

using namespace metal;

#include "motion_vectors.h"

// Writes the motion vector of the moved draw items' visible surface
// (render::MotionVectorPass). Clip positions are interpolated
// perspective-correctly, so dividing them here gives the exact screen
// position of this fragment's surface point in both frames.

// Must match ObjectMotionVaryings in motion_object.vert.metal.
struct ObjectMotionVaryings {
    float4 position [[position]];
    float4 clip [[user(loc0)]];
    float4 previousClip [[user(loc1)]];
};

fragment float4 FragmentMain(ObjectMotionVaryings in [[stage_in]])
{
    return float4(MotionVector(in.clip, in.previousClip), 0.0, 0.0);
}
