#version 450

// Renders one face of a cube map from an equirect panorama
// (render::Environment). Texels brighter than maxLuminance are scaled down
// to it (the sun clamp for lighting; FLT_MAX for the sky).

#include "environment.glsl"
#include "sanitize.glsl"

// RGBA32F, unfiltered: filtered here by hand (u wraps, v clamps).
layout(set = 0, binding = 0) uniform sampler2D equirect;

layout(push_constant) uniform ToCubeConstants {
    int face;
    float maxLuminance;
    int width;
    int height;
} push;

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

vec3 Fetch(int x, int y)
{
    x = (x % push.width + push.width) % push.width;
    y = clamp(y, 0, push.height - 1);
    return texelFetch(equirect, ivec2(x, y), 0).rgb;
}

void main()
{
    const vec2 uv = EquirectUv(CubeFaceDirection(push.face, inUv));
    const vec2 texel = uv * vec2(push.width, push.height) - 0.5;
    const ivec2 base = ivec2(floor(texel));
    const vec2 f = texel - vec2(base);
    const vec3 top = mix(Fetch(base.x, base.y), Fetch(base.x + 1, base.y), f.x);
    const vec3 bottom = mix(Fetch(base.x, base.y + 1), Fetch(base.x + 1, base.y + 1), f.x);
    vec3 radiance = mix(top, bottom, f.y);
    const float luminance = dot(radiance, kLuminanceWeights);
    if (luminance > push.maxLuminance) {
        radiance *= push.maxLuminance / luminance;
    }
    // Half floats end at 65504; keep the sun finite (and a broken texel of
    // the panorama out of every mip and prefiltered level built from it).
    outColor = vec4(SanitizeColor(radiance, kMaxHalfRadiance), 1.0);
}
