#include <metal_stdlib>

#include "bindings.h"

using namespace metal;

#include "common.h"
#include "scope_lens.h"

// The scope eyepiece's part of the temporal scaler's reactive mask
// (render::ScopeLensPass::DrawReactive()): 1 over the whole glass. What the
// glass shows (the scope's picture, the reticle, the eye-box shadow) moves
// with the weapon and the aim, not with the surface behind it whose motion
// vector its pixels carry, so the scaler must not blend history into it.

fragment float4 FragmentMain(ScopeLensVaryings varyings [[stage_in]])
{
    const float lensRadius = length(varyings.lensPosition);
    const float rimWidth = max(fwidth(lensRadius), 1.0e-6);
    // The antialiased rim of scope_lens.frag.metal: the alpha of its blend.
    const float window = 1.0 - smoothstep(1.0 - rimWidth, 1.0, lensRadius);
    // Additive (the mask saturates at 1): only r is stored.
    return float4(window, 0.0, 0.0, 0.0);
}
