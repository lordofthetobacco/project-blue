#include <metal_stdlib>

#include "bindings.h"

using namespace metal;

// Outline of the selection mask (render::OverlayPass): pixels outside the
// mask within kRadius pixels of it get the selection colour, at full
// opacity next to visible parts and dimmed next to hidden ones.

// Must match OutlineConstants in src/render/passes/overlay_pass.mm.
struct OutlineConstants {
    float4 color;  // display space; alpha = opacity
};

// Must match FullscreenVaryings in fullscreen.vert.metal.
struct FullscreenVaryings {
    float4 position [[position]];
    float2 uv [[user(loc0)]];
};

constant int kRadius = 2;
constant float kHiddenOpacity = 0.35;

fragment float4 FragmentMain(FullscreenVaryings varyings [[stage_in]],
                             // R8: 0 = not selected, 0.5 = selected but
                             // hidden, 1 = selected, visible.
                             texture2d<float> selectionMask [[texture(kTextureInput0)]],
                             constant OutlineConstants& params [[buffer(kBufferPass)]])
{
    const int2 size = int2(selectionMask.get_width(), selectionMask.get_height());
    const int2 center = int2(varyings.uv * float2(size));
    if (selectionMask.read(uint2(center)).r > 0.25) {
        discard_fragment();  // Inside the selection: no outline.
    }
    float nearest = 0.0;
    for (int y = -kRadius; y <= kRadius; ++y) {
        for (int x = -kRadius; x <= kRadius; ++x) {
            if (x * x + y * y > kRadius * kRadius + 1) {
                continue;
            }
            const int2 texel = clamp(center + int2(x, y), int2(0), size - 1);
            nearest = max(nearest, selectionMask.read(uint2(texel)).r);
        }
    }
    if (nearest < 0.25) {
        discard_fragment();
    }
    const float opacity = nearest > 0.75 ? 1.0 : kHiddenOpacity;
    return float4(params.color.rgb, params.color.a * opacity);
}
