// Velocity of the moving draw items (render MotionBlurPass): their position
// this frame and last frame in clip space, written as blur vector and
// coverage of their visible surface into the velocity target. Clip
// positions are interpolated perspective-correctly, so dividing them in the
// fragment stage gives the exact screen position of the fragment's surface
// point in both frames. Program: motion_velocity.

#include "common.slang"
#include "motion_blur.slang"

// Must match VelocityConstants in src/render/vulkan/passes/motion_blur_pass.cc.
struct VelocityConstants {
    float4x4 model;
    float4x4 previousModel;  // The item's model matrix last frame.
};
BLUE_PUSH_CONSTANTS(VelocityConstants, draw);

struct Varyings {
    // Tested EQUAL against the depth pre-pass (depth.slang): both call
    // ClipPosition().
    float4 clipPosition : SV_Position;
    float4 currentClip : TEXCOORD0;
    float4 previousClip : TEXCOORD1;
};

[shader("vertex")]
Varyings VertexMain([[vk::location(0)]] float3 position : POSITION)
{
    Varyings output;
    float4 worldPosition;
    output.clipPosition = ClipPosition(draw.model, position, worldPosition);
    output.currentClip = output.clipPosition;
    output.previousClip =
        mul(frame.previousViewProj, mul(draw.previousModel, float4(position, 1.0)));
    return output;
}

[shader("fragment")]
float4 FragmentMain(Varyings input) : SV_Target
{
    const float2 velocity =
        ScreenVelocity(input.currentClip, input.previousClip, frame.screen.xy);
    const float maxLength = frame.motionBlur.y;
    return EncodeBlur(BlurVector(velocity, frame.motionBlur.x, maxLength), maxLength);
}
