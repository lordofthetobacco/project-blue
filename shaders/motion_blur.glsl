// Weapon motion blur (render::MotionBlurPass): blur vectors and their
// encoding in the velocity target. Included with #include "motion_blur.glsl".
//
// ScreenVelocity() and BlurVector() have a CPU copy of the same names in
// src/render/motion_blur_math.h (tested there), expression for expression:
// keep the two in sync. The blur vector of a pixel is where its surface point
// moves on screen during the exposure, in pixels (x right, y down); the point
// is smeared from -blur / 2 to +blur / 2 around it.
#ifndef BLUE_SHADERS_MOTION_BLUR_GLSL_
#define BLUE_SHADERS_MOTION_BLUR_GLSL_

// Must match render::kMotionBlurDeadZonePixels.
const float kMotionBlurDeadZonePixels = 0.5;
// Must match render::kMotionBlurMinClipW.
const float kMotionBlurMinClipW = 1.0e-6;

// Returns how far a surface point moved on screen since the previous frame,
// in pixels (x right, y down), from its clip-space position this frame and
// last frame, on a target of `extent` pixels whose clip-space y points up.
// Zero when either position is behind its camera or the result is not
// finite.
vec2 ScreenVelocity(vec4 currentClip, vec4 previousClip, vec2 extent)
{
    if (!(currentClip.w > kMotionBlurMinClipW) || !(previousClip.w > kMotionBlurMinClipW)) {
        return vec2(0.0);
    }
    const vec2 delta = (currentClip.xy / currentClip.w - previousClip.xy / previousClip.w) * 0.5 * extent;
    const vec2 velocity = vec2(delta.x, -delta.y);
    return any(isnan(velocity)) || any(isinf(velocity)) ? vec2(0.0) : velocity;
}

// Returns the blur vector of a point moving by `velocity` pixels per frame:
// velocity * scale, dropped below the dead zone, ramped up from there to
// twice the dead zone, and clamped to `maxLength` pixels.
vec2 BlurVector(vec2 velocity, float scale, float maxLength)
{
    const vec2 blur = velocity * scale;
    const float len = length(blur);
    if (!(len >= kMotionBlurDeadZonePixels) || isinf(len)) {
        return vec2(0.0);
    }
    const float ramped = len < 2.0 * kMotionBlurDeadZonePixels
                             ? (len - kMotionBlurDeadZonePixels) * 2.0
                             : len;
    return blur * (min(ramped, maxLength) / len);
}

// The velocity target (A2B10G10R10_UNORM): rg = blur / maxLength * 0.5 + 0.5,
// a = coverage (1 on the moving items, 0 elsewhere; cleared to "no motion").
vec4 EncodeBlur(vec2 blur, float maxLength)
{
    return vec4(clamp(blur / maxLength * 0.5 + 0.5, 0.0, 1.0), 0.0, 1.0);
}

// Returns the blur vector of a velocity texel written by EncodeBlur(). The
// 10-bit encoding has steps of maxLength / 511.5 pixels and cannot hold 0
// exactly: a still point decodes to about maxLength / 1023 (0.05 px at
// 1080p), which the blur pass treats as no motion.
vec2 DecodeBlur(vec4 texel, float maxLength)
{
    return (texel.xy * 2.0 - 1.0) * maxLength;
}

// Returns true where a velocity texel lies on a moving item.
bool IsCovered(vec4 texel)
{
    return texel.a > 0.5;
}

#endif  // BLUE_SHADERS_MOTION_BLUR_GLSL_
