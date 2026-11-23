// Tile max of the velocity target (render MotionBlurPass): one output texel
// per tile of tileSize x tileSize pixels holds the longest blur vector of
// the covered pixels in it, in pixels (0 where nothing moves). Pixels
// outside `area` hold no moving item and are skipped.
// Program: motion_tile_max.

#include "fullscreen.slang"
#include "motion_blur.slang"

BLUE_TEXTURE(Texture2D<float4>, velocityImage, 0, BLUE_INPUT_SET);

// Must match TileMaxConstants in src/render/vulkan/passes/motion_blur_pass.cc.
struct TileMaxConstants {
    int4 area;        // Pixels to look at: xy min, zw max (exclusive).
    int tileSize;     // Pixels per tile side.
    float maxLength;  // Longest blur vector in pixels (DecodeBlur).
};
BLUE_PUSH_CONSTANTS(TileMaxConstants, params);

[shader("fragment")]
float4 FragmentMain(FullscreenVaryings input) : SV_Target
{
    const int2 tileOrigin = int2(input.position.xy) * params.tileSize;
    const int2 first = max(tileOrigin, params.area.xy);
    const int2 last = min(tileOrigin + params.tileSize, params.area.zw);
    float2 longest = float2(0.0);
    float longestSq = 0.0;
    for (int y = first.y; y < last.y; ++y) {
        for (int x = first.x; x < last.x; ++x) {
            const float4 texel = velocityImage.Load(int3(x, y, 0));
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
