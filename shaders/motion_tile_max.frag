#version 450

#include "motion_blur.glsl"

// Tile max of the velocity target (render::MotionBlurPass): one output texel
// per tile of tileSize x tileSize pixels holds the longest blur vector of
// the covered pixels in it, in pixels (0 where nothing moves). Pixels
// outside `area` hold no moving item and are skipped.

layout(set = 0, binding = 0) uniform sampler2D velocityImage;

// Must match TileMaxConstants in src/render/vulkan/passes/motion_blur_pass.cc.
layout(push_constant) uniform TileMaxConstants {
    ivec4 area;       // Pixels to look at: xy min, zw max (exclusive).
    int tileSize;     // Pixels per tile side.
    float maxLength;  // Longest blur vector in pixels (DecodeBlur).
} params;

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outTileMax;

void main()
{
    const ivec2 tileOrigin = ivec2(gl_FragCoord.xy) * params.tileSize;
    const ivec2 first = max(tileOrigin, params.area.xy);
    const ivec2 last = min(tileOrigin + params.tileSize, params.area.zw);
    vec2 longest = vec2(0.0);
    float longestSq = 0.0;
    for (int y = first.y; y < last.y; ++y) {
        for (int x = first.x; x < last.x; ++x) {
            const vec4 texel = texelFetch(velocityImage, ivec2(x, y), 0);
            const vec2 blur = DecodeBlur(texel, params.maxLength);
            const float lengthSq = dot(blur, blur);
            if (IsCovered(texel) && lengthSq > longestSq) {
                longest = blur;
                longestSq = lengthSq;
            }
        }
    }
    outTileMax = vec4(longest, 0.0, 0.0);
}
