#include <metal_stdlib>

#include "bindings.h"

using namespace metal;

#include "environment.h"
#include "sanitize.h"

// Renders one face of a cube map from an equirect panorama
// (render::Environment). Texels brighter than maxLuminance are scaled down
// to it (the sun clamp for lighting; FLT_MAX for the sky).

// Must match ToCubeConstants in src/render/environment.mm.
struct ToCubeConstants {
    int face;
    float maxLuminance;
    int width;
    int height;
};

// Must match FullscreenVaryings in fullscreen.vert.metal.
struct FullscreenVaryings {
    float4 position [[position]];
    float2 uv [[user(loc0)]];
};

// RGBA32Float, unfiltered: filtered here by hand (u wraps, v clamps).
static float3 Fetch(texture2d<float> equirect, constant ToCubeConstants& pass, int x, int y)
{
    x = (x % pass.width + pass.width) % pass.width;
    y = clamp(y, 0, pass.height - 1);
    return equirect.read(uint2(x, y)).rgb;
}

fragment float4 FragmentMain(FullscreenVaryings varyings [[stage_in]],
                             texture2d<float> equirect [[texture(kTextureInput0)]],
                             constant ToCubeConstants& pass [[buffer(kBufferPass)]])
{
    const float2 uv = EquirectUv(CubeFaceDirection(pass.face, varyings.uv));
    const float2 texel = uv * float2(pass.width, pass.height) - 0.5;
    const int2 base = int2(floor(texel));
    const float2 f = texel - float2(base);
    const float3 top = mix(Fetch(equirect, pass, base.x, base.y),
                           Fetch(equirect, pass, base.x + 1, base.y), f.x);
    const float3 bottom = mix(Fetch(equirect, pass, base.x, base.y + 1),
                              Fetch(equirect, pass, base.x + 1, base.y + 1), f.x);
    float3 radiance = mix(top, bottom, f.y);
    const float luminance = dot(radiance, kLuminanceWeights);
    if (luminance > pass.maxLuminance) {
        radiance *= pass.maxLuminance / luminance;
    }
    // Half floats end at 65504; keep the sun finite (and a broken texel of
    // the panorama out of every mip and prefiltered level built from it).
    return float4(SanitizeColor(radiance, kMaxHalfRadiance), 1.0);
}
