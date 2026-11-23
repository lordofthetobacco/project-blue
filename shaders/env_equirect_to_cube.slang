// Renders one face of a cube map from an equirect panorama (render
// Environment). Texels brighter than maxLuminance are scaled down to it (the
// sun clamp for lighting; FLT_MAX for the sky).
// Program: env_equirect_to_cube.

#include "environment.slang"
#include "fullscreen.slang"
#include "sanitize.slang"

// RGBA32F, unfiltered: filtered here by hand (u wraps, v clamps).
BLUE_TEXTURE(Texture2D<float4>, equirect, 0, BLUE_INPUT_SET);

// Must match ToCubeConstants in src/render/vulkan/environment.cc.
struct ToCubeConstants {
    int face;
    float maxLuminance;
    int width;
    int height;
};
BLUE_PUSH_CONSTANTS(ToCubeConstants, push);

float3 Fetch(int x, int y)
{
    x = (x % push.width + push.width) % push.width;
    y = clamp(y, 0, push.height - 1);
    return equirect.Load(int3(x, y, 0)).rgb;
}

[shader("fragment")]
float4 FragmentMain(FullscreenVaryings input) : SV_Target
{
    const float2 uv = EquirectUv(CubeFaceDirection(push.face, input.uv));
    const float2 texel = uv * float2(float(push.width), float(push.height)) - 0.5;
    const int2 base = int2(floor(texel));
    const float2 f = texel - float2(base);
    const float3 top = lerp(Fetch(base.x, base.y), Fetch(base.x + 1, base.y), f.x);
    const float3 bottom = lerp(Fetch(base.x, base.y + 1), Fetch(base.x + 1, base.y + 1), f.x);
    float3 radiance = lerp(top, bottom, f.y);
    const float luminance = dot(radiance, kLuminanceWeights);
    if (luminance > push.maxLuminance) {
        radiance *= push.maxLuminance / luminance;
    }
    // Half floats end at 65504; keep the sun finite (and a broken texel of
    // the panorama out of every mip and prefiltered level built from it).
    return float4(SanitizeColor(radiance, kMaxHalfRadiance), 1.0);
}
