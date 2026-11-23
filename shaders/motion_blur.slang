// Weapon motion blur (render MotionBlurPass): blur vectors and their
// encoding in the velocity target. Included with
// #include "motion_blur.slang"; safe to include more than once.
//
// ScreenVelocity() and BlurVector() have a CPU copy of the same names in
// src/render/motion_blur_math.h (tested there), expression for expression:
// keep the two in sync. The blur vector of a pixel is where its surface point
// moves on screen during the exposure, in pixels (x right, y down); the point
// is smeared from -blur / 2 to +blur / 2 around it.
#ifndef BLUE_SHADERS_MOTION_BLUR_SLANG_
#define BLUE_SHADERS_MOTION_BLUR_SLANG_

// Must match render::kMotionBlurDeadZonePixels.
static const float kMotionBlurDeadZonePixels = 0.5;
// Must match render::kMotionBlurMinClipW.
static const float kMotionBlurMinClipW = 1.0e-6;

// Returns how far a surface point moved on screen since the previous frame,
// in pixels (x right, y down), from its clip-space position this frame and
// last frame, on a target of `extent` pixels whose clip-space y points up.
// Zero when either position is behind its camera or the result is not
// finite.
float2 ScreenVelocity(float4 currentClip, float4 previousClip, float2 extent)
{
    if (!(currentClip.w > kMotionBlurMinClipW) || !(previousClip.w > kMotionBlurMinClipW)) {
        return float2(0.0);
    }
    const float2 delta =
        (currentClip.xy / currentClip.w - previousClip.xy / previousClip.w) * 0.5 * extent;
    const float2 velocity = float2(delta.x, -delta.y);
    return any(isnan(velocity)) || any(isinf(velocity)) ? float2(0.0) : velocity;
}

// Returns the blur vector of a point moving by `velocity` pixels per frame:
// velocity * scale, dropped below the dead zone, ramped up from there to
// twice the dead zone, and clamped to `maxLength` pixels.
float2 BlurVector(float2 velocity, float scale, float maxLength)
{
    const float2 blur = velocity * scale;
    const float len = length(blur);
    if (!(len >= kMotionBlurDeadZonePixels) || isinf(len)) {
        return float2(0.0);
    }
    const float ramped = len < 2.0 * kMotionBlurDeadZonePixels
                             ? (len - kMotionBlurDeadZonePixels) * 2.0
                             : len;
    return blur * (min(ramped, maxLength) / len);
}

// The velocity target (A2B10G10R10_UNORM): rg = blur / maxLength * 0.5 + 0.5,
// a = coverage (1 on the moving items, 0 elsewhere; cleared to "no motion").
float4 EncodeBlur(float2 blur, float maxLength)
{
    return float4(clamp(blur / maxLength * 0.5 + 0.5, 0.0, 1.0), 0.0, 1.0);
}

// Returns the blur vector of a velocity texel written by EncodeBlur(). The
// 10-bit encoding has steps of maxLength / 511.5 pixels and cannot hold 0
// exactly: a still point decodes to about maxLength / 1023 (0.05 px at
// 1080p), which the blur pass treats as no motion.
float2 DecodeBlur(float4 texel, float maxLength)
{
    return (texel.xy * 2.0 - 1.0) * maxLength;
}

// Returns true where a velocity texel lies on a moving item.
bool IsCovered(float4 texel)
{
    return texel.a > 0.5;
}

#endif  // BLUE_SHADERS_MOTION_BLUR_SLANG_
