// Outline of the selection mask (render OverlayPass): pixels outside the
// mask within kRadius pixels of it get the selection colour, at full
// opacity next to visible parts and dimmed next to hidden ones.
// Program: selection_outline.

#include "fullscreen.slang"

// R8: 0 = not selected, 0.5 = selected but hidden, 1 = selected, visible.
BLUE_TEXTURE(Texture2D<float4>, selectionMask, 0, BLUE_INPUT_SET);

// Must match OutlineConstants in src/render/vulkan/passes/overlay_pass.cc.
struct OutlineConstants {
    float4 color;  // display space; alpha = opacity
};
BLUE_PUSH_CONSTANTS(OutlineConstants, params);

static const int kRadius = 2;
static const float kHiddenOpacity = 0.35;

[shader("fragment")]
float4 FragmentMain(FullscreenVaryings input) : SV_Target
{
    uint width;
    uint height;
    selectionMask.GetDimensions(width, height);
    const int2 size = int2(int(width), int(height));
    const int2 center = int2(input.uv * float2(size));
    if (selectionMask.Load(int3(center, 0)).r > 0.25) {
        discard;  // Inside the selection: no outline.
    }
    float nearest = 0.0;
    for (int y = -kRadius; y <= kRadius; ++y) {
        for (int x = -kRadius; x <= kRadius; ++x) {
            if (x * x + y * y > kRadius * kRadius + 1) {
                continue;
            }
            const int2 texel = clamp(center + int2(x, y), int2(0), size - 1);
            nearest = max(nearest, selectionMask.Load(int3(texel, 0)).r);
        }
    }
    if (nearest < 0.25) {
        discard;
    }
    const float opacity = nearest > 0.75 ? 1.0 : kHiddenOpacity;
    return float4(params.color.rgb, params.color.a * opacity);
}
