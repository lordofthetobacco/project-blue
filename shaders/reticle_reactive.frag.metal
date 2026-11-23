#include <metal_stdlib>

#include "bindings.h"

using namespace metal;

#include "common.h"
#include "reticle.h"

// The reflex sight's part of the temporal scaler's reactive mask
// (render::ReticlePass::DrawReactive()): 1 where the dot is. The dot is an
// image at infinity that moves with the weapon, not with the surface behind
// the window whose motion vector its pixels carry, so the scaler must not
// blend history into it. The rest of the window is see-through and writes
// 0: what shows through it has valid motion vectors and stays anti-aliased.

fragment float4 FragmentMain(ReticleVaryings varyings [[stage_in]],
                             constant FrameUniforms& frame [[buffer(kBufferFrame)]],
                             constant ReticleConstants& sight [[buffer(kBufferPass)]])
{
    const ReticleCoverage coverage =
        ReticleCoverageOf(varyings, frame.cameraPosition.xyz, sight);
    // Additive (the mask saturates at 1): only r is stored.
    return float4(saturate(coverage.dot * coverage.window), 0.0, 0.0, 0.0);
}
