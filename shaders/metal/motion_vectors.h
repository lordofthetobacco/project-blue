// The scene's motion vectors for the temporal scaler
// (render::MotionVectorPass). Included with #include "motion_vectors.h"
// after <metal_stdlib>; safe to include more than once. Functions are
// qualified with metal:: so that the header does not depend on the including
// file's `using namespace metal;`.
#ifndef BLUE_SHADERS_METAL_MOTION_VECTORS_H_
#define BLUE_SHADERS_METAL_MOTION_VECTORS_H_

// Must match render::kMotionVectorMinClipW: clip-space w at or below which a
// point counts as behind the camera.
constant float kMotionVectorMinClipW = 1.0e-6;

// Returns the motion vector of a surface point that is at `clip` this frame
// and was at `previousClip` in the previous one (both in clip space, both
// from unjittered view-projections): the offset from where it is now to
// where it was, in UV units of the target (x right, y down, 1 = the whole
// width or height). Zero when either point is not in front of the camera or
// the result is not finite.
//
// A copy of render::MotionVector() in src/render/temporal_math.h (tested
// there), expression for expression: keep the two in sync.
static inline float2 MotionVector(float4 clip, float4 previousClip)
{
    if (!(clip.w > kMotionVectorMinClipW) || !(previousClip.w > kMotionVectorMinClipW)) {
        return float2(0.0);
    }
    const float2 ndc = clip.xy / clip.w;
    const float2 previousNdc = previousClip.xy / previousClip.w;
    const float2 motion = (previousNdc - ndc) * float2(0.5, -0.5);
    return metal::any(metal::isnan(motion)) || metal::any(metal::isinf(motion)) ? float2(0.0) : motion;
}

#endif  // BLUE_SHADERS_METAL_MOTION_VECTORS_H_
