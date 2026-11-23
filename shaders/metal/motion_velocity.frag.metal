#include <metal_stdlib>

#include "bindings.h"

using namespace metal;

#include "common.h"
#include "motion_blur.h"

// Writes the blur vector and coverage of the moving draw items' visible
// surface into the velocity target (render::MotionBlurPass). Clip positions
// are interpolated perspective-correctly, so dividing them here gives the
// exact screen position of this fragment's surface point in both frames.

// Must match VelocityVaryings in motion_velocity.vert.metal.
struct VelocityVaryings {
    float4 position [[position]];
    float4 currentClip [[user(loc0)]];
    float4 previousClip [[user(loc1)]];
};

fragment float4 FragmentMain(VelocityVaryings in [[stage_in]],
                             constant FrameUniforms& frame [[buffer(kBufferFrame)]])
{
    const float2 velocity = ScreenVelocity(in.currentClip, in.previousClip, frame.screen.xy);
    const float maxLength = frame.motionBlur.y;
    return EncodeBlur(BlurVector(velocity, frame.motionBlur.x, maxLength), maxLength);
}
