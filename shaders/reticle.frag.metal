#include <metal_stdlib>

#include "bindings.h"

using namespace metal;

#include "common.h"
#include "reticle.h"
#include "sanitize.h"

// A collimated (reflex) sight: the dot is an image at infinity along the
// optic axis, so whether a pixel shows it depends only on the direction from
// the eye through that pixel, never on where on the lens the pixel lies. That
// alone gives true parallax behaviour: with the eye off the axis the dot
// slides across the lens yet stays on the same distant point, and once the
// axis direction falls outside the window there is no fragment to light.

fragment float4 FragmentMain(ReticleVaryings varyings [[stage_in]],
                             constant FrameUniforms& frame [[buffer(kBufferFrame)]],
                             constant ReticleConstants& sight [[buffer(kBufferPass)]])
{
    const float lensRadius = length(varyings.lensPosition);
    const float rimWidth = max(fwidth(lensRadius), 1.0e-6);
    const float3 direction = normalize(varyings.worldPosition - frame.cameraPosition.xyz);
    // Angle one pixel spans, from how fast the direction changes on screen.
    const float pixelAngle = max(length(fwidth(direction)), 1.0e-7);

    // Antialiased rim of the window; zero outside the disc. No discard: the
    // pass is additive, so black adds nothing, and a fragment shader that
    // can discard keeps the tile GPU from removing hidden surfaces early.
    const float window = 1.0 - smoothstep(1.0 - rimWidth, 1.0, lensRadius);

    // Angle to the axis; the chord form stays accurate for tiny angles where
    // acos(dot) loses all precision.
    const float angle = 2.0 * asin(min(0.5 * length(direction - sight.axis.xyz), 1.0));
    const float radius = max(sight.axis.w, 0.5 * sight.dotColor.w * pixelAngle);
    const float dotMask =
        1.0 - smoothstep(radius - 0.5 * pixelAngle, radius + 0.5 * pixelAngle, angle);

    // Additive: alpha is ignored by the blend.
    return float4(SanitizeRadiance((sight.dotColor.rgb * dotMask + sight.tint.rgb) * window), 0.0);
}
