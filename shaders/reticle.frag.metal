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
    // No discard outside the disc: the pass is additive, so black adds
    // nothing, and a fragment shader that can discard keeps the tile GPU
    // from removing hidden surfaces early.
    const ReticleCoverage coverage =
        ReticleCoverageOf(varyings, frame.cameraPosition.xyz, sight);

    // Additive: alpha is ignored by the blend.
    return float4(SanitizeRadiance((sight.dotColor.rgb * coverage.dot + sight.tint.rgb) *
                                   coverage.window),
                  0.0);
}
