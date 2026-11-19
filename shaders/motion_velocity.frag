#version 450

#include "common.glsl"
#include "motion_blur.glsl"

// Writes the blur vector and coverage of the moving draw items' visible
// surface into the velocity target (render::MotionBlurPass). Clip positions
// are interpolated perspective-correctly, so dividing them here gives the
// exact screen position of this fragment's surface point in both frames.

layout(location = 0) in vec4 inCurrentClip;
layout(location = 1) in vec4 inPreviousClip;

layout(location = 0) out vec4 outVelocity;

void main()
{
    const vec2 velocity = ScreenVelocity(inCurrentClip, inPreviousClip, frame.screen.xy);
    const float maxLength = frame.motionBlur.y;
    outVelocity = EncodeBlur(BlurVector(velocity, frame.motionBlur.x, maxLength), maxLength);
}
