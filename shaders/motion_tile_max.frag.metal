#include <metal_stdlib>

#include "bindings.h"

using namespace metal;

#include "motion_blur.h"

// Tile max of the velocity target (render::MotionBlurPass): one output texel
// per tile of tileSize x tileSize pixels holds the longest blur vector of
// the covered pixels in it, in pixels (0 where nothing moves). Pixels
// outside `area` hold no moving item and are skipped.

// Must match TileMaxConstants in src/render/passes/motion_blur_pass.mm.
struct TileMaxConstants {
    int4 area;        // Pixels to look at: xy min, zw max (exclusive).
    int tileSize;     // Pixels per tile side.
    float maxLength;  // Longest blur vector in pixels (DecodeBlur).
};
static_assert(sizeof(TileMaxConstants) == 32,
              "TileMaxConstants must match src/render/passes/motion_blur_pass.mm");

// Must match FullscreenVaryings in fullscreen.vert.metal.
struct FullscreenVaryings {
    float4 position [[position]];
    float2 uv [[user(loc0)]];
};

fragment float4 FragmentMain(FullscreenVaryings varyings [[stage_in]],
                             texture2d<float> velocityImage [[texture(kTextureInput0)]],
                             constant TileMaxConstants& params [[buffer(kBufferPass)]])
{
    const int2 tileOrigin = int2(varyings.position.xy) * params.tileSize;
    const int2 first = max(tileOrigin, params.area.xy);
    const int2 last = min(tileOrigin + params.tileSize, params.area.zw);
    float2 longest = float2(0.0);
    float longestSq = 0.0;
    for (int y = first.y; y < last.y; ++y) {
        for (int x = first.x; x < last.x; ++x) {
            const float4 texel = velocityImage.read(uint2(x, y));
            const float2 blur = DecodeBlur(texel, params.maxLength);
            const float lengthSq = dot(blur, blur);
            if (IsCovered(texel) && lengthSq > longestSq) {
                longest = blur;
                longestSq = lengthSq;
            }
        }
    }
    return float4(longest, 0.0, 0.0);
}
